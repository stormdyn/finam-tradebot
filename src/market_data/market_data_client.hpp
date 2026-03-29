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

namespace finam::market_data {

// Колбэки для стримов — вызываются из фонового потока
using QuoteCallback    = std::function<void(const Quote&)>;
using BarCallback      = std::function<void(const Bar&)>;
using OrderBookCallback = std::function<void(const OrderBook&)>;

// RAII-хэндл подписки — деструктор останавливает стрим
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
        std::string_view  timeframe,   // "M1","M5","M15","H1","D1"
        Timestamp         from,
        Timestamp         to
    );

    // Стрим котировок (bid/ask/last) — неблокирующий
    [[nodiscard]] SubscriptionHandle subscribe_quotes(
        std::vector<Symbol> symbols,
        QuoteCallback       callback
    );

    // Стрим свечей в реальном времени — неблокирующий
    [[nodiscard]] SubscriptionHandle subscribe_bars(
        const Symbol&  symbol,
        std::string_view timeframe,
        BarCallback    callback
    );

    // Стрим стакана — неблокирующий
    [[nodiscard]] SubscriptionHandle subscribe_order_book(
        const Symbol&    symbol,
        OrderBookCallback callback
    );

private:
    // Запускает поток чтения стрима, возвращает RAII-хэндл
    template<typename Stub, typename Req, typename Resp, typename Parser>
    SubscriptionHandle make_stream_subscription(
        Req request,
        Parser parser
    );

    [[nodiscard]] std::unique_ptr<grpc::ClientContext> make_context() const;

    std::shared_ptr<auth::TokenManager> token_mgr_;
    std::shared_ptr<grpc::Channel>      channel_;
};

} // namespace finam::market_data
