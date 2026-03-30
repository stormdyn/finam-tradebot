#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <grpcpp/grpcpp.h>

#include "core/interfaces.hpp"
#include "auth/token_manager.hpp"
#include "strategy/ofi_types.hpp"   // TradeEvent, BookLevelEvent

namespace finam::market_data {

using QuoteCallback     = std::function<void(const Quote&)>;
using BarCallback       = std::function<void(const Bar&)>;
using OrderBookCallback = std::function<void(const OrderBook&)>;
// Новый: исполненные сделки из SubscribeLatestTrades
using TradeCallback     = std::function<void(const strategy::TradeEvent&)>;
// Новый: дельты уровней стакана (распарсенный diff двух OrderBook снапшотов)
using BookEventCallback = std::function<void(const strategy::BookLevelEvent&)>;

class Subscription {
public:
    explicit Subscription(std::function<void()> cancel)
        : cancel_(std::move(cancel)) {}
    ~Subscription() { cancel_(); }

    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;

private:
    std::function<void()> cancel_;
};

using SubscriptionHandle = std::unique_ptr<Subscription>;

class MarketDataClient {
public:
    explicit MarketDataClient(std::shared_ptr<auth::TokenManager> token_mgr);
    ~MarketDataClient();

    MarketDataClient(const MarketDataClient&)            = delete;
    MarketDataClient& operator=(const MarketDataClient&) = delete;

    // Исторические свечи — блокирующий вызов
    [[nodiscard]] Result<std::vector<Bar>> get_bars(
        const Symbol&     symbol,
        std::string_view  timeframe,
        Timestamp         from,
        Timestamp         to
    );

    // Стрим котировок (bid/ask/last)
    [[nodiscard]] SubscriptionHandle subscribe_quotes(
        std::vector<Symbol> symbols,
        QuoteCallback       callback
    );

    // Стрим свечей в реальном времени
    [[nodiscard]] SubscriptionHandle subscribe_bars(
        const Symbol&    symbol,
        std::string_view timeframe,
        BarCallback      callback
    );

    // Стрим стакана — raw снапшоты (для отображения/логов)
    [[nodiscard]] SubscriptionHandle subscribe_order_book(
        const Symbol&     symbol,
        OrderBookCallback callback
    );

    // Стрим исполненных сделок — для TFI/CVD
    [[nodiscard]] SubscriptionHandle subscribe_latest_trades(
        const Symbol&  symbol,
        TradeCallback  callback
    );

    // Стрим дельт стакана — для MLOFI
    // Внутри сравнивает последовательные снапшоты и эмитит BookLevelEvent
    // на каждое изменение уровня 0..kBookLevels-1
    [[nodiscard]] SubscriptionHandle subscribe_book_events(
        const Symbol&     symbol,
        BookEventCallback callback
    );

private:
    [[nodiscard]] std::unique_ptr<grpc::ClientContext> make_context() const;

    std::shared_ptr<auth::TokenManager> token_mgr_;
    std::shared_ptr<grpc::Channel>      channel_;
};

} // namespace finam::market_data