#pragma once
#include <chrono>
#include <thread>
#include <atomic>
#include <spdlog/spdlog.h>

namespace finam::core {

// ── MaintenanceWindow ────────────────────────────────────────────────────────────
//
// MOEX FORTS техобслуживание: 05:00–06:15 MSK ежедневно.
// MSK = UTC+3, поэтому окно: [02:00, 03:15) UTC.
//
// is_active():  true если сейчас в окне техобслуживания.
// wait_if_active(): блокирует пока в окне, затем возвращает.
// Прерывается сразу если stop = true.

struct MaintenanceConfig {
    int start_utc_min{120};  // 02:00 UTC (05:00 MSK)
    int end_utc_min{195};    // 03:15 UTC (06:15 MSK)
    std::chrono::seconds poll_interval{30};
};

class MaintenanceWindow {
public:
    explicit MaintenanceWindow(MaintenanceConfig cfg = {}) noexcept
        : cfg_(cfg) {}

    [[nodiscard]] bool is_active() const noexcept {
        return in_window(utc_min_now());
    }

    // Блокирует до выхода из окна или stop.
    // Возвращает: true — вышли из окна, false — остановка по stop
    template<typename StopFlag>
    [[nodiscard]] bool wait_if_active(const StopFlag& stop) noexcept {
        if (!is_active()) return true;
        spdlog::warn("[Maintenance] window active (05:00-06:15 MSK), waiting...");
        while (!stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(cfg_.poll_interval);
            if (!is_active()) {
                spdlog::info("[Maintenance] window ended, resuming");
                return true;
            }
        }
        return false;
    }

private:
    [[nodiscard]] static int utc_min_now() noexcept {
        const auto now = std::chrono::system_clock::now();
        const auto tt  = std::chrono::system_clock::to_time_t(now);
        return static_cast<int>((tt % 86400) / 60);
    }

    [[nodiscard]] bool in_window(int utc_min) const noexcept {
        return utc_min >= cfg_.start_utc_min && utc_min < cfg_.end_utc_min;
    }

    MaintenanceConfig cfg_;
};

} // namespace finam::core
