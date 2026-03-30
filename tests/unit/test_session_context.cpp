#include <catch2/catch_test_macros.hpp>
#include "strategy/session_context.hpp"

using namespace finam;
using namespace finam::strategy;
using namespace std::chrono_literals;

// Строим бар с заданными OHLCV
static Bar make_bar(double o, double h, double l, double c,
                    std::string_view tf = "D1")
{
    return Bar{
        .symbol    = Symbol{"Si-6.26", "FORTS"},
        .open      = o, .high = h, .low = l, .close = c,
        .volume    = 1000,
        .ts        = std::chrono::system_clock::now(),
        .timeframe = std::string(tf),
    };
}

TEST_CASE("SessionContext: ATR computes after first bar", "[session]") {
    SessionContext ctx{};
    ctx.on_daily_bar(make_bar(100, 110, 95, 108));  // range=15
    CHECK(ctx.atr() > 0.0);
}

TEST_CASE("SessionContext: NR7 detected after 7 bars", "[session]") {
    SessionContext ctx{};
    // 6 широких баров
    for (int i = 0; i < 6; ++i)
        ctx.on_daily_bar(make_bar(100, 120, 80, 110));  // range=40
    // 7-й бар уже имеет range < предыдущих
    ctx.on_daily_bar(make_bar(100, 105, 98, 103));  // range=7
    CHECK(ctx.nr7_confirmed());
}

TEST_CASE("SessionContext: NR7 not detected when last bar is widest", "[session]") {
    SessionContext ctx{};
    for (int i = 0; i < 6; ++i)
        ctx.on_daily_bar(make_bar(100, 105, 98, 103));  // range=7
    ctx.on_daily_bar(make_bar(100, 130, 70, 110));  // range=60 — шире всех
    CHECK_FALSE(ctx.nr7_confirmed());
}

TEST_CASE("SessionContext: ORB bias set after finalize", "[session]") {
    SessionContext ctx{};
    ctx.on_daily_bar(make_bar(100, 120, 80, 115));  // ATR есть
    ctx.session_reset();
    ctx.finalize_orb(125.0);  // mid > high D1 => Long bias
    CHECK(ctx.orb_finalized());
    CHECK(ctx.allows_long());
    CHECK_FALSE(ctx.allows_short());
}

TEST_CASE("SessionContext: size_multiplier > 1 with NR7", "[session]") {
    SessionContext ctx{};
    for (int i = 0; i < 6; ++i)
        ctx.on_daily_bar(make_bar(100, 120, 80, 110));
    ctx.on_daily_bar(make_bar(100, 105, 98, 103));
    CHECK(ctx.size_multiplier() > 1.0f);
}
