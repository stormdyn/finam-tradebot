#include "market_data_client.hpp"

#include <map>
#include <cmath>
#include <chrono>
#include <spdlog/spdlog.h>

// После git submodule update --init --recursive
// сгенерированные файлы окажутся в build/ (cmake генерирует их из proto)
#include "grpc/tradeapi/v1/marketdata/marketdata_service.grpc.pb.h"
#include "grpc/tradeapi/v1/marketdata/marketdata_service.pb.h"

namespace finam::market_data {

// ── Decimal helpers ───────────────────────────────────────────────────────────

namespace {

// google.type.Decimal — строковое представление числа ("85000.5")
double decimal_to_double(const google::type::Decimal& d) noexcept {
    try { return std::stod(d.value()); }
    catch (...) { return 0.0; }
}

// Конвертация строки таймфрейма → proto enum
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
    // Формат: "Si-6.26@FORTS" → security_code="Si-6.26", security_board="FORTS"
    const auto at = s.rfind('@');
    if (at == std::string::npos) return Symbol{s, ""};
    return Symbol{s.substr(0, at), s.substr(at + 1)};
}

} // namespace

// ── MarketDataClient ──────────────────────────────────────────────────────────

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

// ── get_bars ──────────────────────────────────────────────────────────────────

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

    const auto from_t = std::chrono::system_clock::to_time_t(from);
    const auto to_t   = std::chrono::system_clock::to_time_t(to);
    req.mutable_interval()->mutable_start_time()->set_seconds(from_t);
    req.mutable_interval()->mutable_end_time()->set_seconds(to_t);

    auto ctx = make_context();
    BarsResponse resp;
    const auto status = stub_->Bars(ctx.get(), req, &resp);

    if (!status.ok()) {
        spdlog::error("[MD] get_bars failed: {}", status.error_message());
        return std::unexpected(Error{ErrorCode::RpcError, status.error_message()});
    }

    std::vector<Bar> bars;
    bars.reserve(resp.bars_size());
    for (const auto& b : resp.bars()) {
        bars.push_back(Bar{
            .symbol = symbol,
            .open   = decimal_to_double(b.open()),
            .high   = decimal_to_double(b.high()),
            .low    = decimal_to_double(b.low()),
            .close  = decimal_to_double(b.close()),
            .volume = static_cast<int64_t>(decimal_to_double(b.volume())),
            .ts     = ts_from_proto(b.timestamp()),
        });
    }

    spdlog::info("[MD] get_bars got {} bars", bars.size());
    return bars;
}

// ── subscribe_quotes ──────────────────────────────────────────────────────────

SubscriptionHandle MarketDataClient::subscribe_quotes(
    std::vector<Symbol> symbols,
    QuoteCallback       callback)
{
    using namespace ::grpc::tradeapi::v1::marketdata;

    auto stop = std::make_shared<std::atomic<bool>>(false);

    auto thread = std::thread([this,
                               symbols  = std::move(symbols),
                               callback = std::move(callback),
                               stop]() mutable
    {
        spdlog::info("[MD] subscribe_quotes started ({} symbols)", symbols.size());

        while (!stop->load(std::memory_order_acquire)) {
            auto ctx = make_context();

            SubscribeQuoteRequest req;
            for (const auto& s : symbols)
                req.add_symbols(s.to_string());

            auto stream = stub_->SubscribeQuote(ctx.get(), req);

            SubscribeQuoteResponse resp;
            while (!stop->load() && stream->Read(&resp)) {
                if (resp.has_error() && resp.error().code() != 0) {
                    spdlog::warn("[MD] quote stream error: {}",
                        resp.error().description());
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
            }
            stream->Finish();

            if (!stop->load()) {
                spdlog::warn("[MD] subscribe_quotes reconnecting in 1s...");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        spdlog::info("[MD] subscribe_quotes stopped");
    });

    thread.detach();
    return std::make_unique<Subscription>([stop]() {
        stop->store(true, std::memory_order_release);
    });
}

// ── subscribe_bars ────────────────────────────────────────────────────────────

SubscriptionHandle MarketDataClient::subscribe_bars(
    const Symbol&    symbol,
    std::string_view timeframe,
    BarCallback      callback)
{
    using namespace ::grpc::tradeapi::v1::marketdata;

    auto stop    = std::make_shared<std::atomic<bool>>(false);
    auto sym_str = symbol.to_string();
    auto tf      = timeframe_to_proto(timeframe);

    auto thread = std::thread([this, sym_str, tf,
                               callback = std::move(callback), stop]() mutable
    {
        spdlog::info("[MD] subscribe_bars started symbol={}", sym_str);

        while (!stop->load(std::memory_order_acquire)) {
            auto ctx = make_context();

            SubscribeBarsRequest req;
            req.set_symbol(sym_str);
            req.set_timeframe(tf);

            auto stream = stub_->SubscribeBars(ctx.get(), req);

            SubscribeBarsResponse resp;
            const Symbol sym = symbol_from_string(sym_str);
            while (!stop->load() && stream->Read(&resp)) {
                for (const auto& b : resp.bars()) {
                    callback(Bar{
                        .symbol = sym,
                        .open   = decimal_to_double(b.open()),
                        .high   = decimal_to_double(b.high()),
                        .low    = decimal_to_double(b.low()),
                        .close  = decimal_to_double(b.close()),
                        .volume = static_cast<int64_t>(decimal_to_double(b.volume())),
                        .ts     = ts_from_proto(b.timestamp()),
                    });
                }
            }
            stream->Finish();

            if (!stop->load()) {
                spdlog::warn("[MD] subscribe_bars reconnecting in 1s...");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        spdlog::info("[MD] subscribe_bars stopped");
    });

    thread.detach();
    return std::make_unique<Subscription>([stop]() {
        stop->store(true, std::memory_order_release);
    });
}

// ── subscribe_order_book ──────────────────────────────────────────────────────
//
// SubscribeOrderBookResponse содержит repeated StreamOrderBook.
// Каждый StreamOrderBook::Row несёт Action (ADD/UPDATE/REMOVE) и oneof side
// (buy_size / sell_size) — это инкрементальный стрим, не снапшоты.
//
// protobuf генерирует enum-значения с полным flat-префиксом:
//   StreamOrderBook_Row_Action_ACTION_REMOVE  (числовое значение = 1)
//   StreamOrderBook_Row_Action_ACTION_ADD     (= 2)
//   StreamOrderBook_Row_Action_ACTION_UPDATE  (= 3)
// using-алиас Action = StreamOrderBook::Row::Action даёт доступ через
// Action::ACTION_REMOVE — но ТОЛЬКО если компилятор поддерживает это.
// Для надёжности используем числовой cast через static_cast<int>.

SubscriptionHandle MarketDataClient::subscribe_order_book(
    const Symbol&     symbol,
    OrderBookCallback callback)
{
    using namespace ::grpc::tradeapi::v1::marketdata;

    auto stop    = std::make_shared<std::atomic<bool>>(false);
    auto sym_str = symbol.to_string();

    auto thread = std::thread([this, sym_str,
                               callback = std::move(callback), stop]() mutable
    {
        spdlog::info("[MD] subscribe_order_book started symbol={}", sym_str);

        std::map<double, OrderBookRow, std::greater<double>> bids_map;
        std::map<double, OrderBookRow> asks_map;

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
                        // ACTION_UNSPECIFIED=0, ACTION_REMOVE=1,
                        // ACTION_ADD=2, ACTION_UPDATE=3
                        const int act = static_cast<int>(row.action());
                        constexpr int kRemove = 1;

                        if (row.has_buy_size()) {
                            const double qty = decimal_to_double(row.buy_size());
                            if (act == kRemove || qty < 1e-9)
                                bids_map.erase(price);
                            else
                                bids_map[price] = OrderBookRow{
                                    price,
                                    static_cast<int64_t>(qty)
                                };
                        } else if (row.has_sell_size()) {
                            const double qty = decimal_to_double(row.sell_size());
                            if (act == kRemove || qty < 1e-9)
                                asks_map.erase(price);
                            else
                                asks_map[price] = OrderBookRow{
                                    price,
                                    static_cast<int64_t>(qty)
                                };
                        }
                    }

                    OrderBook book;
                    book.ts     = std::chrono::system_clock::now();
                    book.symbol = symbol_from_string(sym_str);
                    for (auto& [p, r] : bids_map) book.bids.push_back(r);
                    for (auto& [p, r] : asks_map) book.asks.push_back(r);
                    callback(book);
                }
            }
            stream->Finish();

            if (!stop->load()) {
                spdlog::warn("[MD] subscribe_order_book reconnecting in 1s...");
                bids_map.clear();
                asks_map.clear();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        spdlog::info("[MD] subscribe_order_book stopped");
    });

    thread.detach();
    return std::make_unique<Subscription>([stop]() {
        stop->store(true, std::memory_order_release);
    });
}

// ── subscribe_latest_trades ───────────────────────────────────────────────────
//
// Side enum из proto/grpc/tradeapi/v1/side.proto:
//   SIDE_BUY  → is_buy = true
//   SIDE_SELL → is_buy = false
//   SIDE_UNSPECIFIED → tick rule (price >= last_price)

SubscriptionHandle MarketDataClient::subscribe_latest_trades(
    const Symbol&  symbol,
    TradeCallback  callback)
{
    using namespace ::grpc::tradeapi::v1::marketdata;
    using Side = ::grpc::tradeapi::v1::Side;

    auto stop    = std::make_shared<std::atomic<bool>>(false);
    auto sym_str = symbol.to_string();

    auto thread = std::thread([this, sym_str,
                               callback = std::move(callback), stop]() mutable
    {
        spdlog::info("[MD] subscribe_latest_trades started symbol={}", sym_str);

        double last_price = 0.0;

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
                    if (t.side() == Side::SIDE_BUY)
                        is_buy = true;
                    else if (t.side() == Side::SIDE_SELL)
                        is_buy = false;
                    else
                        // Tick rule fallback для SIDE_UNSPECIFIED
                        is_buy = (price >= last_price);

                    last_price = price;

                    callback(strategy::TradeEvent{
                        .price  = price,
                        .volume = vol,
                        .is_buy = is_buy,
                        .ts     = ts_from_proto(t.timestamp()),
                    });
                }
            }
            stream->Finish();

            if (!stop->load()) {
                spdlog::warn("[MD] subscribe_latest_trades reconnecting in 1s...");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        spdlog::info("[MD] subscribe_latest_trades stopped");
    });

    thread.detach();
    return std::make_unique<Subscription>([stop]() {
        stop->store(true, std::memory_order_release);
    });
}

// ── subscribe_book_events ─────────────────────────────────────────────────────
// Адаптер над subscribe_order_book: диффит последовательные снапшоты
// и эмитит BookLevelEvent для каждого изменившегося уровня 0..kBookLevels-1.

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

        prev->bids = book.bids;
        prev->asks = book.asks;
    };

    return subscribe_order_book(symbol, std::move(on_book));
}

} // namespace finam::market_data
