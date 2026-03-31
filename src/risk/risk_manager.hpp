#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <core/interfaces.hpp>
#include <auth/token_manager.hpp>

namespace finam::risk {

struct RiskConfig {
    double  per_trade_pct      = 2.0;
    double  max_daily_loss_pct = 5.0;
    double  max_drawdown_pct   = 15.0;
    int32_t max_positions      = 3;
    bool    require_stop_loss  = true;
    int32_t rollover_days      = 5;
};

struct AccountState {
    double  liquid_value   = 0.0;
    double  used_margin    = 0.0;
    double  free_margin    = 0.0;
    double  daily_pnl      = 0.0;
    double  open_positions = 0.0;
    std::chrono::system_clock::time_point updated_at;
};

class RiskManager final : public IRiskManager {
public:
    explicit RiskManager(
        RiskConfig                          cfg,
        std::shared_ptr<auth::TokenManager> token_mgr
    );
    ~RiskManager() override;

    void start(std::chrono::seconds poll_interval = std::chrono::seconds{10});
    void shutdown() noexcept;

    Result<void> check(const OrderRequest& req) override;

    // Принимает OrderUpdate — сам считает PnL по fill_price * qty * side.
    // Вызывается из StrategyRunner::make_order_callback() до push в очередь.
    // Требует qty_filled > 0 и status == Filled|PartialFill.
    void on_fill(const OrderUpdate& upd) noexcept;

    void trip_circuit_breaker(std::string_view reason) noexcept;
    [[nodiscard]] bool is_tripped() const noexcept;
    [[nodiscard]] AccountState account_state() const;

private:
    [[nodiscard]] Result<double> fetch_initial_margin(
        const Symbol& sym, bool is_long) const;
    Result<void> refresh_account();
    void poll_loop(std::chrono::seconds interval);

    Result<void> check_circuit_breaker()  const noexcept;
    Result<void> check_daily_loss()       const noexcept;
    Result<void> check_drawdown()         const noexcept;
    Result<void> check_position_count()   const noexcept;
    Result<void> check_margin(const OrderRequest& req);
    Result<void> check_expiry(const Symbol& sym) const noexcept;

    RiskConfig                          cfg_;
    std::shared_ptr<auth::TokenManager> token_mgr_;

    mutable std::shared_mutex mu_;
    AccountState              state_;
    double                    peak_liquid_ = 0.0;

    // Отслеживаем открытые позиции для подсчёта PnL по on_fill()
    // key = transaction_id (local_id OrderClient)
    struct OpenPos {
        double  entry_price;
        int32_t qty;    // > 0 long, < 0 short
    };
    std::unordered_map<int32_t, OpenPos> open_pos_;  // protected by mu_

    std::atomic<bool> tripped_{false};
    std::string       trip_reason_;

    std::thread       poll_thread_;
    std::atomic<bool> stop_{false};
};

} // namespace finam::risk
