#pragma once
#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <variant>
#include <spdlog/spdlog.h>
#include "auth/token_manager.hpp"
#include "risk/risk_manager.hpp"
#include "core/contract.hpp"

#include "confluence_strategy.hpp"
#include "core/spsc_queue.hpp"
#include "market_data/market_data_client.hpp"
#include "core/interfaces.hpp"

namespace finam::strategy {

// ── StrategyRunner ───────────────────────────────────────────────────────────
//
// Threading model:
//   gRPC MD threads  →  queue_.push()            →  strategy_thread_
//   OrderClient      →  queue_.push(OrderUpdate)  →  strategy_thread_
//   rollover_thread_ →  queue_.push(RolloverEvent) →  strategy_thread_
//
// Роллирование:
//   Отдельный поток каждую минуту вызывает nearest_contract().
//   Если символ изменился — пушим RolloverEvent в ту же SPSC-очередь.
//   strategy_thread_ получает событие, отписывается, сбрасывает стратегию,
//   переподписывается — всё строго из одного потока, нет data-race.

class StrategyRunner {
public:
    struct Config {
        ConfluenceStrategy::Config strategy;
        std::string  base_ticker;      // "Si", "RTS", "GOLD", "MIX" — тикер без серии
        int  history_days{20};
        int  rollover_days{5};         // передаётся в nearest_contract()
        std::chrono::microseconds poll_interval{100};
    };

    StrategyRunner(
        Config                                         cfg,
        std::shared_ptr<market_data::MarketDataClient> md,
        std::shared_ptr<IOrderExecutor>                executor,
        std::shared_ptr<auth::TokenManager>            token_mgr,
        std::shared_ptr<risk::RiskManager>             risk = nullptr)
        : strategy_(cfg.strategy)
        , cfg_(cfg)
        , md_(std::move(md))
        , executor_(std::move(executor))
        , token_mgr_(std::move(token_mgr))
        , risk_(std::move(risk))
        , poll_interval_(cfg.poll_interval)
    {
        load_history(cfg);
        start_strategy_thread();
        start_subscriptions(strategy_.symbol());
        start_rollover_watchdog();
    }

    ~StrategyRunner() {
        stop_.store(true, std::memory_order_release);
        if (rollover_thread_.joinable()) rollover_thread_.join();
        if (strategy_thread_.joinable()) strategy_thread_.join();
    }

    StrategyRunner(const StrategyRunner&)            = delete;
    StrategyRunner& operator=(const StrategyRunner&) = delete;

    [[nodiscard]] finam::order::OrderUpdateCallback make_order_callback() {
        return [this](const OrderUpdate& upd) {
            // Filter by symbol: each runner only processes its own orders.
            // In multi-symbol mode a shared OrderClient broadcasts to all
            // runners, so we must discard updates for other instruments.
            if (upd.symbol != active_symbol() && upd.symbol.security_code != "") {
                return;  // not our symbol — skip
            }
            if (risk_ &&
                (upd.status == OrderStatus::Filled ||
                 upd.status == OrderStatus::PartialFill) &&
                upd.qty_filled > 0)
            {
                risk_->on_fill(upd);
            }
            if (!queue_.push(upd))
                spdlog::warn("[Runner] order_queue full, OrderUpdate dropped");
        };
    }

private:
    // ── RolloverEvent ────────────────────────────────────────────────────────────
    struct RolloverEvent { Symbol new_symbol; };

    using MdEvent = std::variant<
        BookLevelEvent, TradeEvent, Quote, OrderUpdate, RolloverEvent>;

    static constexpr std::size_t kQueueSize = 1024;
    using Queue = core::SpscQueue<MdEvent, kQueueSize>;

    // ── Rollover watchdog ─────────────────────────────────────────────────────────

    void start_rollover_watchdog() {
        rollover_thread_ = std::thread([this] {
            spdlog::debug("[Runner] rollover watchdog started");
            while (!stop_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::minutes{1});
                if (stop_.load(std::memory_order_acquire)) break;
                check_rollover();
            }
        });
    }

    void check_rollover() {
        const auto next = core::nearest_contract(
            base_ticker_, cfg_.rollover_days);
        // active_symbol_ читается только из rollover_thread_, пишется из strategy_thread_
        // → используем atomic<Symbol> через string-атом (copy under mutex)
        const Symbol cur = active_symbol();
        if (next.security_code == cur.security_code) return;

        spdlog::info("[Runner] rollover detected: {} → {}",
            cur.to_string(), next.to_string());

        if (!queue_.push(RolloverEvent{next}))
            spdlog::error("[Runner] queue full, rollover event dropped!");
    }

    // ── Strategy thread ────────────────────────────────────────────────────────────

    void start_strategy_thread() {
        strategy_thread_ = std::thread([this] {
            spdlog::info("[Runner] strategy thread started");
            while (!stop_.load(std::memory_order_acquire)) {
                drain_queue();
                std::this_thread::sleep_for(poll_interval_);
            }
            drain_queue();
            spdlog::info("[Runner] strategy thread stopped");
        });
    }

    void drain_queue() noexcept {
        while (auto event = queue_.pop())
            std::visit([this](auto&& e) { dispatch(e); }, *event);
    }

    void dispatch(const BookLevelEvent& e) noexcept {
        if (auto sig = strategy_.on_book_event(e)) handle_signal(*sig);
    }
    void dispatch(const TradeEvent& e) noexcept {
        if (auto sig = strategy_.on_trade_event(e)) handle_signal(*sig);
    }
    void dispatch(const Quote& q) noexcept {
        strategy_.update_bbo(q.bid, q.ask);
        const auto sig = strategy_.on_quote(q);
        if (sig.direction != Signal::Direction::None) handle_signal(sig);
    }
    void dispatch(const OrderUpdate& upd) noexcept {
        strategy_.on_order_update(upd);
    }

    // ── Rollover handler (strategy thread) ───────────────────────────────────────

    void dispatch(const RolloverEvent& ev) noexcept {
        spdlog::info("[Runner] executing rollover to {}", ev.new_symbol.to_string());

        // 1. Отписываемся от старого символа
        unsubscribe_all();

        // 2. Сбрасываем стратегию (позиция уже закрыта check_expiry в RiskManager)
        strategy_.on_session_open();

        // 3. Обновляем active symbol
        set_active_symbol(ev.new_symbol);

        // 4. Загружаем историю по новому контракту
        Config cfg_copy = cfg_;
        cfg_copy.strategy.symbol = ev.new_symbol;
        load_history(cfg_copy);

        // 5. Подписываемся на новый символ
        start_subscriptions(ev.new_symbol);
        spdlog::info("[Runner] rollover complete, active={}",
            ev.new_symbol.to_string());
    }

    // ── Subscriptions ────────────────────────────────────────────────────────────────

    void start_subscriptions(const Symbol& sym) {
        book_sub_ = md_->subscribe_book_events(sym,
            [this](const BookLevelEvent& e) {
                if (!queue_.push(e))
                    spdlog::warn("[Runner] book_queue full");
            });
        trade_sub_ = md_->subscribe_latest_trades(sym,
            [this](const TradeEvent& e) {
                if (!queue_.push(e))
                    spdlog::warn("[Runner] trade_queue full");
            });
        quote_sub_ = md_->subscribe_quotes({sym},
            [this](const Quote& q) {
                if (!queue_.push(q))
                    spdlog::warn("[Runner] quote_queue full");
            });
        m1_bar_sub_ = md_->subscribe_bars(sym, "M1",
            [this](const Bar& bar) { strategy_.on_bar(bar); });
        d1_bar_sub_ = md_->subscribe_bars(sym, "D1",
            [this](const Bar& bar) { strategy_.on_bar(bar); });
        spdlog::info("[Runner] subscribed to {}", sym.to_string());
    }

    void unsubscribe_all() noexcept {
        book_sub_   = {};
        trade_sub_  = {};
        quote_sub_  = {};
        m1_bar_sub_ = {};
        d1_bar_sub_ = {};
    }

    // ── History ──────────────────────────────────────────────────────────────────────

    void load_history(const Config& cfg) {
        const auto to   = std::chrono::system_clock::now();
        const auto from = to - std::chrono::hours(24 * cfg.history_days);
        auto result = md_->get_bars(cfg.strategy.symbol, "D1", from, to);
        if (!result) {
            spdlog::error("[Runner] D1 history load failed: {}",
                result.error().message);
            return;
        }
        for (const auto& bar : *result) strategy_.on_bar(bar);
        spdlog::info("[Runner] loaded {} D1 bars for {}",
            result->size(), cfg.strategy.symbol.to_string());
    }

    // ── Signal execution ───────────────────────────────────────────────────────────

    void handle_signal(const Signal& sig) {
        if (sig.direction == Signal::Direction::None) return;
        const bool is_close = (sig.direction == Signal::Direction::Close);
        OrderRequest req{
            .client_id = std::string(token_mgr_->primary_account_id()),
            .symbol    = sig.symbol,
            .side      = (!is_close && sig.direction == Signal::Direction::Buy)
                         ? OrderSide::Buy : OrderSide::Sell,
            .type      = sig.order_type,
            .price     = sig.price,
            .quantity  = sig.quantity,
        };
        if (risk_ && !is_close) {
            if (auto r = risk_->check(req); !r) {
                spdlog::warn("[Runner] risk REJECTED {}: {}",
                    sig.symbol.to_string(), r.error().message);
                if (r.error().code == ErrorCode::DailyLossLimitHit ||
                    r.error().code == ErrorCode::RiskLimitExceeded)
                    risk_->trip_circuit_breaker(r.error().message);
                return;
            }
        }
        spdlog::info("[Runner] signal {} {} qty={}",
            sig.symbol.to_string(),
            is_close ? "CLOSE" :
            sig.direction == Signal::Direction::Buy ? "BUY" : "SELL",
            sig.quantity);
        if (auto result = executor_->submit(req); !result)
            spdlog::error("[Runner] order rejected: {}", result.error().message);
    }

    // ── Active symbol (shared between rollover_thread_ and strategy_thread_) ──────
    // rollover_thread_ читает active_symbol_str_, strategy_thread_ пишет —
    // защищаем мютексом. Не hot-path — вызывается раз в минуту.
    [[nodiscard]] Symbol active_symbol() const {
        std::lock_guard lock(sym_mu_);
        return active_symbol_;
    }
    void set_active_symbol(const Symbol& s) {
        std::lock_guard lock(sym_mu_);
        active_symbol_ = s;
    }

    // ── Members ──────────────────────────────────────────────────────────────────────

    ConfluenceStrategy                              strategy_;
    Config                                          cfg_;
    std::shared_ptr<market_data::MarketDataClient>  md_;
    std::shared_ptr<IOrderExecutor>                 executor_;
    std::shared_ptr<auth::TokenManager>             token_mgr_;
    std::shared_ptr<risk::RiskManager>              risk_;

    Queue                     queue_;
    std::thread               strategy_thread_;
    std::thread               rollover_thread_;
    std::atomic<bool>         stop_{false};
    std::chrono::microseconds poll_interval_;

    // base ticker (без серии) — для nearest_contract()
    std::string               base_ticker_{cfg_.base_ticker};

    mutable std::mutex        sym_mu_;
    Symbol                    active_symbol_{strategy_.symbol()};

    market_data::SubscriptionHandle  book_sub_;
    market_data::SubscriptionHandle  trade_sub_;
    market_data::SubscriptionHandle  quote_sub_;
    market_data::SubscriptionHandle  m1_bar_sub_;
    market_data::SubscriptionHandle  d1_bar_sub_;
};

} // namespace finam::strategy
