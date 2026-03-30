#include "market_data_client.hpp"

#include <map>
#include <cmath>
#include <chrono>
#include <spdlog/spdlog.h>

#include "core/backoff.hpp"
#include "grpc/tradeapi/v1/marketdata/marketdata_service.grpc.pb.h"
#include "grpc/tradeapi/v1/marketdata/marketdata_service.pb.h"

namespace finam::market_data {

// ── Helpers ──────────────────────────────────────────────────────────────────────────

namespace {

double decimal_to_double(const google::type::Decimal& d) noexcept {
    try { return std::stod(d.value()); }
    catch (...) { return 0.0; }
}

::grpc::tradeapi::v1::marketdata::TimeFrame timeframe_to_proto(
    std::string_view tf) noexcept
{
    using TF = ::grpc::tradeapi::v1::marketdata::TimeFrame;
    if (tf == "M1")  return TF::TIME_FRAME_M1;
    if (tf == "M5")  return TF::TIME_FRAME_M5;
    if (tf == "M15") return TF::TIME_FRAME_M15;
    if (tf == "M30") return TF::TIME_FRAME_M30;
    if (tf == "H1")  return TF::TIME_FRAME_H1;
    if (tf == "H2")  return TF::TIME_FRAME_H2;
    if (tf == "H4")  return TF::TIME_FRAME_H4;
    if (tf == "D1")  return TF::TIME_FRAME_D;
    if (tf == "W1")  return TF::TIME_FRAME_W;
    spdlog::warn("[MD] Unknown timeframe '{}', fallback M1", tf);
    return TF::TIME_FRAME_M1;
}

std::chrono::system_clock::time_point ts_from_proto(
    const google::protobuf::Timestamp& ts) noexcept
{
    return std::chrono::system_clock::from_time_t(ts.seconds())
         + std::chrono::nanoseconds(ts.nanos());
}

Symbol symbol_from_string(const std::string& s) {
    const auto at = s.rfind('@');
    if (at == std::string::npos) return Symbol{s, ""};
    return Symbol{s.substr(0, at), s.substr(at + 1)};
}

// attempt — текущая попытка (0-based), задержка будет base*2^attempt
void log_reconnect(std::string_view name, int attempt) noexcept {
    const int delay_ms = static_cast<int>(
        std::min(500.0 * (1 << std::min(attempt, 6)), 30000.0));
    spdlog::warn("[MD] {} disconnected, reconnect #{} (~{}ms backoff)",
        name, attempt + 1, delay_ms);
}

} // namespace

// ── MarketDataClient ───────────────────────────────────────────────────────────────────

MarketDataClient::MarketDataClient(
    std::shared_ptr<auth::TokenManager> token_mgr)
    : token_mgr_(std::move(token_mgr))
    , channel_(token_mgr_->channel())
{
    stub_ = ::grpc::tradeapi::v1::marketdata::MarketDataService::NewStub(channel_);
    spdlog::debug("[MD] MarketDataClient created");
}

MarketDataClient::~MarketDataClient() {
    spdlog::debug("[MD] MarketDataClient destroyed");
}

std::unique_ptr<grpc::ClientContext> MarketDataClient::make_context() const {
    auto ctx = std::make_unique<grpc::ClientContext>();
    ctx->AddMetadata("authorization", "Bearer " + token_mgr_->jwt());
    return ctx;
}

// ── get_bars ────────────────────────────────────────────────────────────────────────

Result<std::vector<Bar>> MarketDataClient::get_bars(
    const Symbol&    symbol,
    std::string_view timeframe,
    Timestamp        from,
    Timestamp        to)
{
    using namespace ::grpc::tradeapi::v1::marketdata;

    spdlog::info("[MD] get_bars symbol={} tf={}", symbol.to_string(), timeframe);

    BarsRequest req;
    req.set_symbol(symbol.to_string());
    req.set_timeframe(timeframe_to_proto(timeframe));
    req.mutable_interval()->mutable_start_time()->set_seconds(
        std::chrono::system_clock::to_time_t(from));
    req.mutable_interval()->mutable_end_time()->set_seconds(
        std::chrono::system_clock::to_time_t(to));

    auto ctx = make_context();
    BarsResponse resp;
    const auto status = stub_->Bars(ctx.get(), req, &resp);

    if (!status.ok()) {
        spdlog::error("[MD] get_bars failed: {}", status.error_message());
        return std::unexpected(Error{ErrorCode::RpcError, status.error_message()});
    }

    const std::string tf_str{timeframe};
    std::vector<Bar> bars;
    bars.reserve(resp.bars_size());
    for (const auto& b : resp.bars()) {
        bars.push_back(Bar{
            .symbol    = symbol,
            .timeframe = tf_str,
            .open      = decimal_to_double(b.open()),
            .high      = decimal_to_double(b.high()),
            .low       = decimal_to_double(b.low()),
            .close     = decimal_to_double(b.close()),
            .volume    = static_cast<int64_t>(decimal_to_double(b.volume())),
            .ts        = ts_from_proto(b.timestamp()),
        });
    }

    spdlog::info("[MD] get_bars got {} bars ({})", bars.size(), tf_str);
    return bars;
}

// ── subscribe_quotes ──────────────────────────────────────────────────────────────────

SubscriptionHandle MarketDataClient::subscribe_quotes(
    std::vector<Symbol> symbols,
    QuoteCallback       callback)
{
    using namespace ::grpc::tradeapi::v1::marketdata;

    auto stop = std::make_shared<std::atomic<bool>>(false);

    auto thread = std::thread(
        [this, symbols = std::move(symbols), callback = std::move(callback), stop]() mutable
    {
        spdlog::info("[MD] subscribe_quotes started ({} symbols)", symbols.size());
        core::ExponentialBackoff bo;

        while (!stop->load(std::memory_order_acquire)) {
            auto ctx = make_context();
            SubscribeQuoteRequest req;
            for (const auto& s : symbols) req.add_symbols(s.to_string());

            auto stream = stub_->SubscribeQuote(ctx.get(), req);
            SubscribeQuoteResponse resp;

            while (!stop->load() && stream->Read(&resp)) {
                if (resp.has_error() && resp.error().code() != 0) {
                    spdlog::warn("[MD] quote stream error: {}", resp.error().description());
                    break;
                }
                for (const auto& q : resp.quote()) {
                    callback(Quote{
                        .symbol = symbol_from_string(q.symbol()),
                        .bid    = decimal_to_double(q.bid()),
                        .ask    = decimal_to_double(q.ask()),
                        .last   = decimal_to_double(q.last()),
                        .volume = static_cast<int64_t>(decimal_to_double(q.last_size())),
                        .ts     = ts_from_proto(q.timestamp()),
                    });
                }
                bo.reset();  // успешное чтение — сбрасываем backoff
            }
            stream->Finish();

            if (!stop->load()) {
                log_reconnect("subscribe_quotes", bo.attempt());
                if (!bo.wait(*stop)) break;
            }
        }
        spdlog::info("[MD] subscribe_quotes stopped");
    });

    thread.detach();
    return std::make_unique<Subscription>([stop]() {
        stop->store(true, std::memory_order_release);
    });
}

// ── subscribe_bars ───────────────────────────────────────────────────────────────────

SubscriptionHandle MarketDataClient::subscribe_bars(
    const Symbol&    symbol,
    std::string_view timeframe,
    BarCallback      callback)
{
    using namespace ::grpc::tradeapi::v1::marketdata;

    auto stop    = std::make_shared<std::atomic<bool>>(false);
    auto sym_str = symbol.to_string();
    auto tf_str  = std::string{timeframe};
    auto tf      = timeframe_to_proto(timeframe);

    auto thread = std::thread(
        [this, sym_str, tf_str, tf, callback = std::move(callback), stop]() mutable
    {
        spdlog::info("[MD] subscribe_bars started symbol={} tf={}", sym_str, tf_str);
        const Symbol sym = symbol_from_string(sym_str);
        core::ExponentialBackoff bo;

        while (!stop->load(std::memory_order_acquire)) {
            auto ctx = make_context();
            SubscribeBarsRequest req;
            req.set_symbol(sym_str);
            req.set_timeframe(tf);

            auto stream = stub_->SubscribeBars(ctx.get(), req);
            SubscribeBarsResponse resp;

            while (!stop->load() && stream->Read(&resp)) {
                for (const auto& b : resp.bars()) {
                    callback(Bar{
                        .symbol    = sym,
                        .timeframe = tf_str,
                        .open      = decimal_to_double(b.open()),
                        .high      = decimal_to_double(b.high()),
                        .low       = decimal_to_double(b.low()),
                        .close     = decimal_to_double(b.close()),
                        .volume    = static_cast<int64_t>(decimal_to_double(b.volume())),
                        .ts        = ts_from_proto(b.timestamp()),
                    });
                }
                bo.reset();
            }
            stream->Finish();

            if (!stop->load()) {
                log_reconnect("subscribe_bars", bo.attempt());
                if (!bo.wait(*stop)) break;
            }
        }
        spdlog::info("[MD] subscribe_bars stopped");
    });

    thread.detach();
    return std::make_unique<Subscription>([stop]() {
        stop->store(true, std::memory_order_release);
    });
}

// ── subscribe_order_book ───────────────────────────────────────────────────────────────

SubscriptionHandle MarketDataClient::subscribe_order_book(
    const Symbol&     symbol,
    OrderBookCallback callback)
{
    using namespace ::grpc::tradeapi::v1::marketdata;

    auto stop    = std::make_shared<std::atomic<bool>>(false);
    auto sym_str = symbol.to_string();

    auto thread = std::thread(
        [this, sym_str, callback = std::move(callback), stop]() mutable
    {
        spdlog::info("[MD] subscribe_order_book started symbol={}", sym_str);
        std::map<double, OrderBookRow, std::greater<double>> bids_map;
        std::map<double, OrderBookRow>                       asks_map;
        core::ExponentialBackoff bo;

        while (!stop->load(std::memory_order_acquire)) {
            auto ctx = make_context();
            SubscribeOrderBookRequest req;
            req.set_symbol(sym_str);
            auto stream = stub_->SubscribeOrderBook(ctx.get(), req);

            SubscribeOrderBookResponse resp;
            while (!stop->load() && stream->Read(&resp)) {
                for (const auto& sob : resp.order_book()) {
                    for (const auto& row : sob.rows()) {
                        const double price = decimal_to_double(row.price());
                        constexpr int kRemove = 1;
                        const int act = static_cast<int>(row.action());

                        if (row.has_buy_size()) {
                            const double qty = decimal_to_double(row.buy_size());
                            if (act == kRemove || qty < 1e-9) bids_map.erase(price);
                            else bids_map[price] = OrderBookRow{price, static_cast<int64_t>(qty)};
                        } else if (row.has_sell_size()) {
                            const double qty = decimal_to_double(row.sell_size());
                            if (act == kRemove || qty < 1e-9) asks_map.erase(price);
                            else asks_map[price] = OrderBookRow{price, static_cast<int64_t>(qty)};
                        }
                    }

                    OrderBook book;
                    book.ts     = std::chrono::system_clock::now();
                    book.symbol = symbol_from_string(sym_str);
                    for (auto& [p, r] : bids_map) book.bids.push_back(r);
                    for (auto& [p, r] : asks_map) book.asks.push_back(r);
                    callback(book);
                }
                bo.reset();
            }
            stream->Finish();

            if (!stop->load()) {
                log_reconnect("subscribe_order_book", bo.attempt());
                bids_map.clear();
                asks_map.clear();
                if (!bo.wait(*stop)) break;
            }
        }
        spdlog::info("[MD] subscribe_order_book stopped");
    });

    thread.detach();
    return std::make_unique<Subscription>([stop]() {
        stop->store(true, std::memory_order_release);
    });
}

// ── subscribe_latest_trades ───────────────────────────────────────────────────────────

SubscriptionHandle MarketDataClient::subscribe_latest_trades(
    const Symbol&  symbol,
    TradeCallback  callback)
{
    using namespace ::grpc::tradeapi::v1::marketdata;
    using Side = ::grpc::tradeapi::v1::Side;

    auto stop    = std::make_shared<std::atomic<bool>>(false);
    auto sym_str = symbol.to_string();

    auto thread = std::thread(
        [this, sym_str, callback = std::move(callback), stop]() mutable
    {
        spdlog::info("[MD] subscribe_latest_trades started symbol={}", sym_str);
        double last_price = 0.0;
        core::ExponentialBackoff bo;

        while (!stop->load(std::memory_order_acquire)) {
            auto ctx = make_context();
            SubscribeLatestTradesRequest req;
            req.set_symbol(sym_str);
            auto stream = stub_->SubscribeLatestTrades(ctx.get(), req);

            SubscribeLatestTradesResponse resp;
            while (!stop->load() && stream->Read(&resp)) {
                for (const auto& t : resp.trades()) {
                    const double price = decimal_to_double(t.price());
                    const double vol   = decimal_to_double(t.size());

                    bool is_buy;
                    if      (t.side() == Side::SIDE_BUY)  is_buy = true;
                    else if (t.side() == Side::SIDE_SELL) is_buy = false;
                    else                                   is_buy = (price >= last_price);
                    last_price = price;

                    callback(strategy::TradeEvent{
                        .price  = price,
                        .volume = vol,
                        .is_buy = is_buy,
                        .ts     = ts_from_proto(t.timestamp()),
                    });
                }
                bo.reset();
            }
            stream->Finish();

            if (!stop->load()) {
                log_reconnect("subscribe_latest_trades", bo.attempt());
                if (!bo.wait(*stop)) break;
            }
        }
        spdlog::info("[MD] subscribe_latest_trades stopped");
    });

    thread.detach();
    return std::make_unique<Subscription>([stop]() {
        stop->store(true, std::memory_order_release);
    });
}

// ── subscribe_book_events ───────────────────────────────────────────────────────────────

SubscriptionHandle MarketDataClient::subscribe_book_events(
    const Symbol&     symbol,
    BookEventCallback callback)
{
    struct PrevBook {
        std::vector<OrderBookRow> bids;
        std::vector<OrderBookRow> asks;
    };
    auto prev = std::make_shared<PrevBook>();

    auto on_book = [prev, callback = std::move(callback)](
        const OrderBook& book) mutable
    {
        const int  levels = strategy::kBookLevels;
        const auto now    = book.ts;

        for (int k = 0; k < levels; ++k) {
            const double old_bid = (k < static_cast<int>(prev->bids.size()))
                ? static_cast<double>(prev->bids[k].quantity) : 0.0;
            const double old_ask = (k < static_cast<int>(prev->asks.size()))
                ? static_cast<double>(prev->asks[k].quantity) : 0.0;
            const double new_bid = (k < static_cast<int>(book.bids.size()))
                ? static_cast<double>(book.bids[k].quantity) : 0.0;
            const double new_ask = (k < static_cast<int>(book.asks.size()))
                ? static_cast<double>(book.asks[k].quantity) : 0.0;

            if (old_bid == new_bid && old_ask == new_ask) continue;

            const double price =
                (k < static_cast<int>(book.bids.size()))  ? book.bids[k].price  :
                (k < static_cast<int>(book.asks.size()))  ? book.asks[k].price  :
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

        prev->bids = book.bids;
        prev->asks = book.asks;
    };

    return subscribe_order_book(symbol, std::move(on_book));
}

} // namespace finam::market_data
