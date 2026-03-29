#include "market_data_client.hpp"

#include <chrono>
#include <spdlog/spdlog.h>

// Подключаем после успешной компиляции proto:
// #include "grpc/tradeapi/v1/market_data/market_data_service.grpc.pb.h"


namespace finam::market_data {

// ── Helpers ───────────────────────────────────────────────────────────────────

namespace {

// Конвертация строки таймфрейма в proto enum
// TODO: заменить на реальный enum после подключения proto
int timeframe_to_proto(std::string_view tf) {
    if (tf == "M1")  return 1;
    if (tf == "M5")  return 5;
    if (tf == "M15") return 15;
    if (tf == "H1")  return 60;
    if (tf == "D1")  return 1440;
    spdlog::warn("[MD] Unknown timeframe '{}', using M5", tf);
    return 5;
}

} // namespace

// ── MarketDataClient ──────────────────────────────────────────────────────────

MarketDataClient::MarketDataClient(
    std::shared_ptr<auth::TokenManager> token_mgr)
    : token_mgr_(std::move(token_mgr))
    , channel_(token_mgr_->channel())
{
    spdlog::debug("[MD] MarketDataClient created");
}

MarketDataClient::~MarketDataClient() {
    spdlog::debug("[MD] MarketDataClient destroyed");
}

std::unique_ptr<grpc::ClientContext> MarketDataClient::make_context() const {
    auto ctx = std::make_unique<grpc::ClientContext>();
    ctx->AddMetadata("authorization",
        "Bearer " + token_mgr_->jwt());
    return ctx;
}

// ── get_bars (stub) ───────────────────────────────────────────────────────────

Result<std::vector<Bar>> MarketDataClient::get_bars(
    const Symbol&    symbol,
    std::string_view timeframe,
    Timestamp        from,
    Timestamp        to)
{
    spdlog::info("[MD] get_bars symbol={} tf={}", symbol.to_string(), timeframe);

    // TODO: реальный gRPC вызов после подключения proto:
    //
    // auto ctx = make_context();
    // tradeapi::v2::GetBarsRequest req;
    // req.set_symbol(symbol.to_string());
    // req.set_timeframe(timeframe_to_proto(timeframe));
    // req.mutable_interval()->mutable_from()->set_seconds(
    //     std::chrono::system_clock::to_time_t(from));
    // req.mutable_interval()->mutable_to()->set_seconds(
    //     std::chrono::system_clock::to_time_t(to));
    //
    // tradeapi::v2::GetBarsResponse resp;
    // auto status = stub_->GetBars(&ctx, req, &resp);
    // if (!status.ok())
    //     return std::unexpected(Error{ErrorCode::RpcError, status.error_message()});
    //
    // std::vector<Bar> bars;
    // bars.reserve(resp.bars_size());
    // for (const auto& b : resp.bars()) {
    //     bars.push_back(Bar{
    //         .symbol = symbol,
    //         .open   = b.open().value(),
    //         .high   = b.high().value(),
    //         .low    = b.low().value(),
    //         .close  = b.close().value(),
    //         .volume = b.volume(),
    //         .ts     = std::chrono::system_clock::from_time_t(
    //                       b.timestamp().seconds()),
    //     });
    // }
    // return bars;

    // Stub: возвращаем пустой вектор
    return std::vector<Bar>{};
}

// ── subscribe_quotes (stub) ───────────────────────────────────────────────────

SubscriptionHandle MarketDataClient::subscribe_quotes(
    std::vector<Symbol> symbols,
    QuoteCallback       callback)
{
    auto stop = std::make_shared<std::atomic<bool>>(false);

    auto thread = std::thread([this, symbols = std::move(symbols),
                                callback = std::move(callback), stop]()
    {
        spdlog::info("[MD] subscribe_quotes started ({} symbols)", symbols.size());

        // TODO: реальный gRPC стрим после подключения proto:
        //
        // auto ctx = make_context();
        // tradeapi::v2::SubscribeQuotesRequest req;
        // for (const auto& s : symbols)
        //     req.add_symbols(s.to_string());
        // auto stream = stub_->SubscribeQuotes(&ctx, req);
        //
        // tradeapi::v2::SubscribeQuotesResponse resp;
        // while (!stop->load() && stream->Read(&resp)) {
        //     for (const auto& q : resp.quote()) {
        //         callback(Quote{
        //             .symbol = symbol_from_string(q.symbol()),
        //             .bid    = q.bid(),
        //             .ask    = q.ask(),
        //             .last   = q.last(),
        //             .volume = q.last_size(),
        //             .ts     = std::chrono::system_clock::now(),
        //         });
        //     }
        // }
        // stream->Finish();

        // Stub: просто ждём сигнала остановки
        while (!stop->load())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        spdlog::info("[MD] subscribe_quotes stopped");
    });

    thread.detach();

    return std::make_unique<Subscription>([stop]() {
        stop->store(true, std::memory_order_release);
    });
}

// ── subscribe_bars (stub) ─────────────────────────────────────────────────────

SubscriptionHandle MarketDataClient::subscribe_bars(
    const Symbol&    symbol,
    std::string_view timeframe,
    BarCallback      callback)
{
    auto stop = std::make_shared<std::atomic<bool>>(false);

    auto sym_str = symbol.to_string();
    auto thread = std::thread([sym_str, timeframe = std::string(timeframe),
                                callback = std::move(callback), stop]()
    {
        spdlog::info("[MD] subscribe_bars started symbol={} tf={}", sym_str, timeframe);

        while (!stop->load())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        spdlog::info("[MD] subscribe_bars stopped");
    });

    thread.detach();

    return std::make_unique<Subscription>([stop]() {
        stop->store(true, std::memory_order_release);
    });
}

// ── subscribe_order_book (stub) ───────────────────────────────────────────────

SubscriptionHandle MarketDataClient::subscribe_order_book(
    const Symbol&     symbol,
    OrderBookCallback callback)
{
    auto stop = std::make_shared<std::atomic<bool>>(false);

    auto sym_str = symbol.to_string();
    auto thread = std::thread([sym_str,
                                callback = std::move(callback), stop]()
    {
        spdlog::info("[MD] subscribe_order_book started symbol={}", sym_str);

        while (!stop->load())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        spdlog::info("[MD] subscribe_order_book stopped");
    });

    thread.detach();

    return std::make_unique<Subscription>([stop]() {
        stop->store(true, std::memory_order_release);
    });
}

} // namespace finam::market_data
