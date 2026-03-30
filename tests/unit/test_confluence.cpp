#include <catch2/catch_test_macros.hpp>
#include "strategy/confluence_strategy.hpp"

using namespace finam;
using namespace finam::strategy;
using namespace std::chrono_literals;

static ConfluenceStrategy::Config make_cfg() {
    return ConfluenceStrategy::Config{
        .symbol    = Symbol{"Si-6.26", "FORTS"},
        .base_qty  = 1,
        .sl_ticks  = 30.0,
        .tp_ticks  = 90.0,
        .tick_size = 1.0,
    };
}

static Bar make_d1(double o, double h, double l, double c) {
    return Bar{
        .symbol    = Symbol{"Si-6.26", "FORTS"},
        .timeframe = "D1",
        .open = o, .high = h, .low = l, .close = c,
        .volume    = 1000,
        .ts        = std::chrono::system_clock::now(),
    };
}

TEST_CASE("ConfluenceStrategy: SL closes long position", "[confluence]") {
    ConfluenceStrategy strat{make_cfg()};

    // Симулируем заполнение long @ 100
    OrderUpdate fill{
        .side      = OrderSide::Buy,
        .status    = OrderStatus::Filled,
        .price     = 100.0,
        .qty_filled = 1,
    };
    strat.on_order_update(fill);

    // Цена упала на > sl_ticks (30) тиков
    Quote q{
        .symbol = Symbol{"Si-6.26", "FORTS"},
        .bid    = 69.0, .ask = 70.0,
        .ts     = std::chrono::system_clock::now(),
    };
    strat.update_bbo(q.bid, q.ask);
    const auto sig = strat.on_quote(q);
    CHECK(sig.direction == Signal::Direction::Close);
    CHECK(sig.quantity == 1);
}

TEST_CASE("ConfluenceStrategy: TP closes long position", "[confluence]") {
    ConfluenceStrategy strat{make_cfg()};

    OrderUpdate fill{
        .side      = OrderSide::Buy,
        .status    = OrderStatus::Filled,
        .price     = 100.0,
        .qty_filled = 1,
    };
    strat.on_order_update(fill);

    Quote q{
        .symbol = Symbol{"Si-6.26", "FORTS"},
        .bid    = 191.0, .ask = 192.0,  // pnl > tp_ticks (90)
        .ts     = std::chrono::system_clock::now(),
    };
    strat.update_bbo(q.bid, q.ask);
    const auto sig = strat.on_quote(q);
    CHECK(sig.direction == Signal::Direction::Close);
}

TEST_CASE("ConfluenceStrategy: on_bar D1 processed, M1 ignored for bias", "[confluence]") {
    ConfluenceStrategy strat{make_cfg()};
    // D1 бары не генерируют сигнал входа
    const auto sig = strat.on_bar(make_d1(100, 120, 90, 115));
    CHECK(sig.direction == Signal::Direction::None);
}

TEST_CASE("ConfluenceStrategy: no entry without ORB finalized", "[confluence]") {
    ConfluenceStrategy strat{make_cfg()};
    // Без накопленных D1 баров orb не финализирован
    BookLevelEvent e{
        .level        = 0,
        .price        = 100.0,
        .old_bid_size = 0.0, .new_bid_size = 100.0,
        .old_ask_size = 0.0, .new_ask_size = 0.0,
        .ts           = std::chrono::system_clock::now(),
    };
    const auto sig = strat.on_book_event(e);
    CHECK(!sig.has_value() || sig->direction == Signal::Direction::None);
}
