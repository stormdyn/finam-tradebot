#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <core/interfaces.hpp>
#include <auth/token_manager.hpp>

// Forward-declare gRPC generated stubs
namespace grpc::tradeapi::v1::assets {
    class AssetsService;
}
namespace grpc::tradeapi::v1::accounts {
    class AccountsService;
}

namespace finam::risk {

// ── Конфиг ────────────────────────────────────────────────────────────────────
// Все лимиты задаются при создании и не меняются в runtime.
struct RiskConfig {
    double  per_trade_pct     = 2.0;   // % от нетто-ликвидности на 1 сделку
    double  max_daily_loss_pct = 5.0;  // % от нетто-ликвидности
    double  max_drawdown_pct   = 15.0; // % от нетто-ликвидности
    int32_t max_positions      = 3;    // макс. одновременных позиций
    bool    require_stop_loss  = true;
    int32_t rollover_days      = 5;    // за сколько дней до экспирации закрывать
};

// ── Состояние аккаунта (обновляется из AccountService::GetPortfolio) ──────────
struct AccountState {
    double  liquid_value    = 0.0;  // нетто-ликвидность FORTS (руб)
    double  used_margin     = 0.0;  // занятое ГО
    double  free_margin     = 0.0;  // свободное ГО
    double  daily_pnl       = 0.0;  // PnL с начала торговой сессии
    double  open_positions  = 0.0;  // кол-во открытых позиций
    std::chrono::system_clock::time_point updated_at;
};

// ── RiskManager ───────────────────────────────────────────────────────────────
//
// Threading: check() вызывается из strategy thread (один поток).
// refresh_account() вызывается из отдельного polling thread.
// Используем shared_mutex: check() — shared lock, refresh — unique lock.
//
class RiskManager final : public IRiskManager {
public:
    explicit RiskManager(
        RiskConfig                          cfg,
        std::shared_ptr<auth::TokenManager> token_mgr
    );
    ~RiskManager() override;

    // Запускает фоновый поток обновления AccountState (раз в N секунд)
    void start(std::chrono::seconds poll_interval = std::chrono::seconds{10});
    void shutdown() noexcept;

    // IRiskManager — вызывается StrategyRunner перед каждым submit()
    Result<void> check(const OrderRequest& req) override;

    // Обновление после исполнения ордера (вызывает StrategyRunner)
    void on_fill(double pnl_delta) noexcept;

    // Circuit breaker: принудительная остановка торгов
    void trip_circuit_breaker(std::string_view reason) noexcept;
    [[nodiscard]] bool is_tripped() const noexcept;

    // Возвращает копию последнего AccountState (для логов/мониторинга)
    [[nodiscard]] AccountState account_state() const;

private:
    // Получает ГО инструмента через SecurityService::GetTradingInfo
    Result<double> fetch_initial_margin(
        const Symbol& sym, bool is_long) const;

    // Обновляет AccountState из AccountService::GetPortfolio
    Result<void> refresh_account();

    // Фоновый поток
    void poll_loop(std::chrono::seconds interval);

    // Проверки — каждая возвращает Error или пустой Result
    Result<void> check_circuit_breaker()  const noexcept;
    Result<void> check_daily_loss()       const noexcept;
    Result<void> check_drawdown()         const noexcept;
    Result<void> check_position_count()   const noexcept;
    Result<void> check_margin(
        const OrderRequest& req);
    Result<void> check_expiry(
        const Symbol& sym)  const noexcept;

    RiskConfig                          cfg_;
    std::shared_ptr<auth::TokenManager> token_mgr_;

    mutable std::shared_mutex mu_;
    AccountState              state_;       // protected by mu_
    double                    peak_liquid_  = 0.0;  // для drawdown

    std::atomic<bool>   tripped_{false};
    std::string         trip_reason_;       // protected by mu_

    std::thread         poll_thread_;
    std::atomic<bool>   stop_{false};
};

} // namespace finam::risk
