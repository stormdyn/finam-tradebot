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

// ── subscribe_latest_trades ───────────────────────────────────────────────────
//
// SubscribeLatestTrades stream → TradeEvent callback.
//
// Определение buyer/seller-initiated:
//   price >= ask → buyer hit the ask  → is_buy = true
//   price <= bid → seller hit the bid → is_buy = false
//   иначе (mid trade) → классифицируем по направлению tick rule
//
// TODO: после подключения proto заменить stub на реальный gRPC стрим.

SubscriptionHandle MarketDataClient::subscribe_latest_trades(
    const Symbol&  symbol,
    TradeCallback  callback)
{
    auto stop    = std::make_shared<std::atomic<bool>>(false);
    auto sym_str = symbol.to_string();

    auto thread = std::thread([this, sym_str,
                               callback = std::move(callback), stop]()
    {
        spdlog::info("[MD] subscribe_latest_trades started symbol={}", sym_str);

        // TODO: реальный gRPC стрим:
        //
        // auto ctx = make_context();
        // tradeapi::v2::SubscribeLatestTradesRequest req;
        // req.set_symbol(sym_str);
        // auto stream = stub_->SubscribeLatestTrades(&ctx, req);
        //
        // tradeapi::v2::SubscribeLatestTradesResponse resp;
        // double last_price = 0.0;
        // while (!stop->load() && stream->Read(&resp)) {
        //     for (const auto& t : resp.trades()) {
        //         const double price = t.price().num() /
        //                              std::pow(10.0, t.price().scale());
        //         const double vol   = static_cast<double>(t.quantity());
        //         // Tick rule для определения агрессора
        //         const bool is_buy = (price >= last_price);
        //         last_price = price;
        //         callback(strategy::TradeEvent{
        //             .price  = price,
        //             .volume = vol,
        //             .is_buy = is_buy,
        //             .ts     = std::chrono::system_clock::from_time_t(
        //                           t.timestamp().seconds()),
        //         });
        //     }
        // }
        // stream->Finish();

        while (!stop->load())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        spdlog::info("[MD] subscribe_latest_trades stopped");
    });

    thread.detach();

    return std::make_unique<Subscription>([stop]() {
        stop->store(true, std::memory_order_release);
    });
}

// ── subscribe_book_events ─────────────────────────────────────────────────────
//
// Обёртка над subscribe_order_book: диффит последовательные снапшоты
// и эмитит BookLevelEvent для каждого изменившегося уровня 0..kBookLevels-1.
//
// Это ключевой адаптер: OrderBook (снапшот) → BookLevelEvent (инкремент).
// OrderBookState принимает именно инкременты, не снапшоты.

SubscriptionHandle MarketDataClient::subscribe_book_events(
    const Symbol&     symbol,
    BookEventCallback callback)
{
    // Храним предыдущий снапшот внутри лямбды через shared_ptr
    struct PrevBook {
        std::vector<OrderBookRow> bids;
        std::vector<OrderBookRow> asks;
    };
    auto prev = std::make_shared<PrevBook>();

    // Конвертер OrderBook → поток BookLevelEvent
    auto on_book = [prev, callback = std::move(callback)](
        const OrderBook& book) mutable
    {
        const int levels = strategy::kBookLevels;
        const auto now   = book.ts;

        for (int k = 0; k < levels; ++k) {
            const double old_bid = (k < static_cast<int>(prev->bids.size()))
                                   ? prev->bids[k].quantity : 0.0;
            const double old_ask = (k < static_cast<int>(prev->asks.size()))
                                   ? prev->asks[k].quantity : 0.0;
            const double new_bid = (k < static_cast<int>(book.bids.size()))
                                   ? book.bids[k].quantity : 0.0;
            const double new_ask = (k < static_cast<int>(book.asks.size()))
                                   ? book.asks[k].quantity : 0.0;

            // Эмитим только если хоть что-то изменилось
            if (old_bid == new_bid && old_ask == new_ask) continue;

            // Цена уровня: берём из нового снапшота (или старого если исчез)
            const double price =
                (k < static_cast<int>(book.bids.size())) ? book.bids[k].price :
                (k < static_cast<int>(book.asks.size())) ? book.asks[k].price :
                (k < static_cast<int>(prev->bids.size())) ? prev->bids[k].price :
                (k < static_cast<int>(prev->asks.size())) ? prev->asks[k].price :
                0.0;

            callback(strategy::BookLevelEvent{
                .level        = k,
                .price        = price,
                .old_bid_size = old_bid,
                .new_bid_size = new_bid,
                .old_ask_size = old_ask,
                .new_ask_size = new_ask,
                .ts           = now,
            });
        }

        // Обновляем предыдущий снапшот
        prev->bids = book.bids;
        prev->asks = book.asks;
    };

    return subscribe_order_book(symbol, std::move(on_book));
}

} // namespace finam::market_data
