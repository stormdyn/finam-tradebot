#pragma once
#include <string>
#include "core/interfaces.hpp"

namespace finam::risk {

struct RiskConfig {
    double max_daily_loss_rub{50000.0};  // absolute RUB loss limit
    double max_position_margin_pct{0.3}; // max 30% of portfolio in one position
    int    max_open_orders{5};
    double max_leverage{3.0};
};

// Stateful risk manager.
// check() is called by the order pipeline BEFORE submitting to exchange.
// update_pnl() is called on each OrderUpdate (fills).
//
// Thread safety: all public methods guarded by internal mutex.
// Hot-path tradeoff: mutex is fine here — orders are rare vs. quotes.
class RiskManager final : public IRiskManager {
public:
    explicit RiskManager(RiskConfig cfg);

    // IRiskManager
    [[nodiscard]] Result<void> check(const OrderRequest& req) override;

    // Called from order update handler to track realized PnL
    void update_pnl(double delta_rub) noexcept;

    // Reset daily counters (call at session start: 10:00 MSK)
    void reset_daily() noexcept;

private:
    RiskConfig           cfg_;
    double               daily_loss_{0.0};  // negative = loss
    int                  open_orders_{0};
    mutable std::mutex   mu_;
};

} // namespace finam::risk
