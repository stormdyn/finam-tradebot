#pragma once
#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <variant>
#include <spdlog/spdlog.h>
#include "auth/token_manager.hpp"
#include "risk/risk_manager.hpp"

#include "confluence_strategy.hpp"
#include "core/spsc_queue.hpp"
#include "market_data/market_data_client.hpp"
#include "core/interfaces.hpp"

namespace finam::strategy {

// StrategyRunner — владелец стратегии, подписок и strategy thread.
//
// Threading model:
//   gRPC MD thread 1/2  →  push() в queue_  →  strategy_thread_ (consumer)
//   strategy_thread_    →  risk_->check()   →  executor_->submit()
//
// RiskManager::check() — shared_lock внутри, не блокирует polling thread.
class StrategyRunner {
public:
    struct Config {
        ConfluenceStrategy::Config strategy;
        int  history_days{20};
        std::chrono::microseconds poll_interval{100};
    };

    StrategyRunner(
        Config                                         cfg,
        std::shared_ptr<market_data::MarketDataClient> md,
        std::shared_ptr<IOrderExecutor>                executor,
        std::shared_ptr<auth::TokenManager>            token_mgr,
        std::shared_ptr<risk::RiskManager>             risk = nullptr)
        : strategy_(cfg.strategy)
        , md_(std::move(md))
        , executor_(std::move(executor))
        , token_mgr_(std::move(token_mgr))
        , risk_(std::move(risk))
        , poll_interval_(cfg.poll_interval)
    {
        load_history(cfg);
        start_strategy_thread();
        start_subscriptions();
    }

    ~StrategyRunner() {
        stop_.store(true, std::memory_order_release);
        if (strategy_thread_.joinable())
            strategy_thread_.join();
    }

    StrategyRunner(const StrategyRunner&)            = delete;
    StrategyRunner& operator=(const StrategyRunner&) = delete;

private:
    using MdEvent = std::variant<BookLevelEvent, TradeEvent, Quote>;

    static constexpr std::size_t kQueueSize = 1024;
    using Queue = core::SpscQueue<MdEvent, kQueueSize>;

    // ── Strategy thread ──────────────────────────────────────────────────────────

    void start_strategy_thread() {
        strategy_thread_ = std::thread([this] {
            spdlog::info("[Runner] strategy thread started");
            while (!stop_.load(std::memory_order_acquire)) {
                drain_queue();
                std::this_thread::sleep_for(poll_interval_);
            }
            drain_queue();  // финальный дрейн
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

    // ── Subscriptions ─────────────────────────────────────────────────────────────

    void start_subscriptions() {
        const auto& sym = strategy_.symbol();

        book_sub_  = md_->subscribe_book_events(sym,
            [this](const BookLevelEvent& e) {
                if (!queue_.push(e))
                    spdlog::warn("[Runner] book_queue full, event dropped");
            });

        trade_sub_ = md_->subscribe_latest_trades(sym,
            [this](const TradeEvent& e) {
                if (!queue_.push(e))
                    spdlog::warn("[Runner] trade_queue full, event dropped");
            });

        quote_sub_ = md_->subscribe_quotes({sym},
            [this](const Quote& q) {
                if (!queue_.push(q))
                    spdlog::warn("[Runner] quote_queue full, event dropped");
            });

        // M1 бары — редкие, не проходят через queue
        bar_sub_ = md_->subscribe_bars(sym, "M1",
            [this](const Bar& bar) { strategy_.on_bar(bar); });

        spdlog::info("[Runner] subscriptions started");
    }

    // ── History ───────────────────────────────────────────────────────────────────

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
        spdlog::info("[Runner] loaded {} D1 bars", result->size());
    }

    // ── Signal execution (only from strategy_thread_) ──────────────────────

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

        // ── Risk check до отправки ордера ───────────────────────────────
        // Close-сигналы проходят без risk-проверки: закрытие позиции
        // всегда разрешено — даже при circuit breaker
        if (risk_ && !is_close) {
            if (auto r = risk_->check(req); !r) {
                spdlog::warn("[Runner] risk check REJECTED {}: {}",
                    sig.symbol.to_string(), r.error().message);

                // Автотрип circuit breaker при critical ошибках
                if (r.error().code == ErrorCode::DailyLossLimitHit ||
                    r.error().code == ErrorCode::RiskLimitExceeded)
                    risk_->trip_circuit_breaker(r.error().message);

                return;
            }
        }

        spdlog::info("[Runner] signal {} {} qty={}",
            sig.symbol.to_string(),
            sig.direction == Signal::Direction::Buy   ? "BUY"  :
            sig.direction == Signal::Direction::Sell  ? "SELL" :
            sig.direction == Signal::Direction::Close ? "CLOSE": "?",
            sig.quantity);

        auto result = executor_->submit(req);
        if (!result)
            spdlog::error("[Runner] order rejected: {}", result.error().message);
    }

    // ── Members ───────────────────────────────────────────────────────────────────

    ConfluenceStrategy                              strategy_;
    std::shared_ptr<market_data::MarketDataClient>  md_;
    std::shared_ptr<IOrderExecutor>                 executor_;
    std::shared_ptr<auth::TokenManager>             token_mgr_;
    std::shared_ptr<risk::RiskManager>              risk_;      // nullable — без риска тоже работает

    Queue                        queue_;
    std::thread                  strategy_thread_;
    std::atomic<bool>            stop_{false};
    std::chrono::microseconds    poll_interval_;

    market_data::SubscriptionHandle  book_sub_;
    market_data::SubscriptionHandle  trade_sub_;
    market_data::SubscriptionHandle  quote_sub_;
    market_data::SubscriptionHandle  bar_sub_;
};

} // namespace finam::strategy
