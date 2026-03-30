#pragma once
#include <memory>
#include <optional>
#include <spdlog/spdlog.h>

#include "confluence_strategy.hpp"
#include "market_data/market_data_client.hpp"
#include "core/interfaces.hpp"

namespace finam::strategy {

// StrategyRunner — владелец стратегии и всех MD-подписок.
//
// Жизненный цикл:
//   1. Конструктор: загружает историю D1, запускает подписки
//   2. Подписки эмитят события → on_book_event / on_trade_event
//      → std::optional<Signal> → executor_.submit()
//   3. Деструктор: RAII-хэндлы отменяют все подписки
//
// THREADING:
//   - subscribe_book_events и subscribe_latest_trades работают
//     в отдельных потоках (внутри MarketDataClient)
//   - ConfluenceStrategy НЕ потокобезопасна
//   - TODO: добавить SPSC queue между MD потоками и strategy_thread_
//     когда перейдём на реальный gRPC (сейчас стабы — однопоточно)

class StrategyRunner {
public:
    struct Config {
        ConfluenceStrategy::Config strategy;
        int history_days{20}; // сколько D1 баров загружать при старте
    };

    StrategyRunner(
        Config                                       cfg,
        std::shared_ptr<market_data::MarketDataClient> md,
        std::shared_ptr<IOrderExecutor>              executor)
        : strategy_(cfg.strategy)
        , md_(std::move(md))
        , executor_(std::move(executor))
    {
        load_history(cfg);
        start_subscriptions();
    }

    // Деструктор отменяет все подписки через RAII
    ~StrategyRunner() = default;

    StrategyRunner(const StrategyRunner&)            = delete;
    StrategyRunner& operator=(const StrategyRunner&) = delete;

private:
    void load_history(const Config& cfg) {
        const auto to   = std::chrono::system_clock::now();
        const auto from = to - std::chrono::hours(24 * cfg.history_days);

        auto result = md_->get_bars(cfg.strategy.symbol, "D1", from, to);
        if (!result) {
            spdlog::error("[Runner] Failed to load D1 history: {}",
                result.error().message);
            return;
        }
        for (const auto& bar : *result)
            strategy_.on_bar(bar); // SessionContext.on_daily_bar внутри

        spdlog::info("[Runner] Loaded {} D1 bars", result->size());
    }

    void start_subscriptions() {
        const auto& sym = strategy_.symbol(); // добавим геттер ниже

        // Подписка на дельты стакана → MLOFI
        book_sub_ = md_->subscribe_book_events(sym,
            [this](const BookLevelEvent& e) {
                if (auto sig = strategy_.on_book_event(e))
                    handle_signal(*sig);
            });

        // Подписка на сделки → TFI / CVD
        trade_sub_ = md_->subscribe_latest_trades(sym,
            [this](const TradeEvent& e) {
                if (auto sig = strategy_.on_trade_event(e))
                    handle_signal(*sig);
            });

        // Подписка на котировки → BBO обновление + ORB финализация
        quote_sub_ = md_->subscribe_quotes({sym},
            [this](const Quote& q) {
                strategy_.update_bbo(q.bid, q.ask);
                const auto sig = strategy_.on_quote(q);
                if (sig.direction != Signal::Direction::None)
                    handle_signal(sig);
            });

        // Подписка на M1 бары → ORB накопление
        bar_sub_ = md_->subscribe_bars(sym, "M1",
            [this](const Bar& bar) {
                strategy_.on_bar(bar);
            });

        spdlog::info("[Runner] All subscriptions started");
    }

    void handle_signal(const Signal& sig) {
        if (sig.direction == Signal::Direction::None) return;

        spdlog::info("[Runner] Signal: {} {} qty={}",
            sig.symbol.to_string(),
            sig.direction == Signal::Direction::Buy    ? "BUY"   :
            sig.direction == Signal::Direction::Sell   ? "SELL"  :
            sig.direction == Signal::Direction::Close  ? "CLOSE" : "?",
            sig.quantity);

        OrderRequest req{
            .symbol   = sig.symbol,
            .side     = (sig.direction == Signal::Direction::Buy)
                        ? OrderSide::Buy : OrderSide::Sell,
            .type     = sig.order_type,
            .price    = sig.price,
            .quantity = sig.quantity,
        };

        // Close → направление противоположно текущей позиции
        // StrategyRunner не знает позицию — ConfluenceStrategy управляет qty
        if (sig.direction == Signal::Direction::Close)
            req.side = (sig.quantity > 0) ? OrderSide::Sell : OrderSide::Buy;

        auto result = executor_->submit(req);
        if (!result) {
            spdlog::error("[Runner] Order rejected: {}", result.error().message);
        }
    }

    ConfluenceStrategy                              strategy_;
    std::shared_ptr<market_data::MarketDataClient>  md_;
    std::shared_ptr<IOrderExecutor>                 executor_;

    // RAII: деструктор отменяет подписки
    market_data::SubscriptionHandle  book_sub_;
    market_data::SubscriptionHandle  trade_sub_;
    market_data::SubscriptionHandle  quote_sub_;
    market_data::SubscriptionHandle  bar_sub_;
};

} // namespace finam::strategy