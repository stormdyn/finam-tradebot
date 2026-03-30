#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// Тестируем логику проверок напрямую, без gRPC-зависимостей.
// RiskManager сейчас не имеет чистого интерфейса для тестирования без сети,
// поэтому тестируем публичную логику через AccountState + RiskConfig
// через отдельные чистые функции.

#include "risk/risk_manager.hpp"

using namespace finam;
using namespace finam::risk;

// Чистые функции-выдержки из RiskManager (free functions для теста)
// Повторяют check_*() логику без зависимости от gRPC
namespace {

Result<void> check_daily_loss(const RiskConfig& cfg, const AccountState& st) {
    if (st.liquid_value <= 0.0) return {};
    const double pct = (-st.daily_pnl / st.liquid_value) * 100.0;
    if (pct >= cfg.max_daily_loss_pct)
        return std::unexpected(Error{
            ErrorCode::DailyLossLimitHit,
            "daily loss " + std::to_string(pct) + "% >= limit"
        });
    return {};
}

Result<void> check_drawdown(const RiskConfig& cfg,
                             const AccountState& st, double peak)
{
    if (peak <= 0.0) return {};
    const double dd = ((peak - st.liquid_value) / peak) * 100.0;
    if (dd >= cfg.max_drawdown_pct)
        return std::unexpected(Error{
            ErrorCode::RiskLimitExceeded,
            "drawdown " + std::to_string(dd) + "% >= limit"
        });
    return {};
}

Result<void> check_positions(const RiskConfig& cfg, const AccountState& st) {
    if (static_cast<int32_t>(st.open_positions) >= cfg.max_positions)
        return std::unexpected(Error{
            ErrorCode::RiskLimitExceeded,
            "max positions reached"
        });
    return {};
}

} // namespace

TEST_CASE("Risk: daily loss below limit passes", "[risk]") {
    RiskConfig cfg{.max_daily_loss_pct = 5.0};
    AccountState st{.liquid_value = 100'000.0, .daily_pnl = -4'000.0};  // 4%
    CHECK(check_daily_loss(cfg, st).has_value());
}

TEST_CASE("Risk: daily loss at limit fails", "[risk]") {
    RiskConfig cfg{.max_daily_loss_pct = 5.0};
    AccountState st{.liquid_value = 100'000.0, .daily_pnl = -5'000.0};  // 5%
    const auto r = check_daily_loss(cfg, st);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::DailyLossLimitHit);
}

TEST_CASE("Risk: drawdown below limit passes", "[risk]") {
    RiskConfig cfg{.max_drawdown_pct = 15.0};
    AccountState st{.liquid_value = 90'000.0};
    CHECK(check_drawdown(cfg, st, 100'000.0).has_value());  // 10% dd
}

TEST_CASE("Risk: drawdown at limit fails", "[risk]") {
    RiskConfig cfg{.max_drawdown_pct = 15.0};
    AccountState st{.liquid_value = 85'000.0};   // 15% drawdown
    const auto r = check_drawdown(cfg, st, 100'000.0);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::RiskLimitExceeded);
}

TEST_CASE("Risk: position count below limit passes", "[risk]") {
    RiskConfig cfg{.max_positions = 3};
    AccountState st{.open_positions = 2.0};
    CHECK(check_positions(cfg, st).has_value());
}

TEST_CASE("Risk: position count at limit fails", "[risk]") {
    RiskConfig cfg{.max_positions = 3};
    AccountState st{.open_positions = 3.0};
    const auto r = check_positions(cfg, st);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::RiskLimitExceeded);
}
