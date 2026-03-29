#pragma once
#include <string>
#include <memory>
#include <functional>
#include "core/interfaces.hpp"

namespace finam::market_data {

// Callback types for market data events
using QuoteHandler = std::function<void(Quote)>;
using BarHandler   = std::function<void(Bar)>;

struct MarketDataConfig {
    std::string endpoint;
    std::string symbol;          // e.g. "Si-6.26@FORTS"
    // Bar interval in seconds (60 = 1-min bars)
    int bar_interval_seconds{60};
};

// Subscribes to SubscribeQuotes and SubscribeBars streams.
// Reconnects automatically on disconnect (Stream TTL = 86400s).
// Thread model: runs its own gRPC completion queue thread;
//               calls handlers from that thread — handlers must be
//               thread-safe (push to SPSCEventBus, never block).
class MarketDataClient {
public:
    MarketDataClient(MarketDataConfig cfg,
                     std::shared_ptr<grpc::Channel> channel,
                     QuoteHandler on_quote,
                     BarHandler   on_bar);
    ~MarketDataClient();

    [[nodiscard]] Result<void> start();
    void stop() noexcept;

private:
    void run_quote_stream();
    void run_bar_stream();

    MarketDataConfig               cfg_;
    std::shared_ptr<grpc::Channel> channel_;
    QuoteHandler                   on_quote_;
    BarHandler                     on_bar_;
    std::atomic<bool>              stop_{false};
    std::thread                    quote_thread_;
    std::thread                    bar_thread_;
};

} // namespace finam::market_data
