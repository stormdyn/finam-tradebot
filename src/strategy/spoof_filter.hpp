#pragma once
#include <chrono>
#include <unordered_map>
#include <cmath>
#include "ofi_types.hpp"

namespace finam::strategy {

// Детектор spoofing/layering на уровнях стакана.
//
// Логика: для каждого price level отслеживаем:
//   - кол-во появлений крупной заявки (ADD с размером > threshold)
//   - кол-во отмен до исполнения (DELETE без предшествующего FILL)
//
// Если cancel_ratio = cancels / (cancels + fills) > max_cancel_ratio
// на протяжении spoof_window → уровень помечается ненадёжным.
//
// Ненадёжный уровень исключается из расчёта MLOFI в OrderBookState.
//
// Вызывать из strategy thread вместе с OrderBookState.
class SpoofFilter {
public:
    struct Config {
        double                    min_large_qty{10.0};    // порог "крупной" заявки (лотов)
        double                    max_cancel_ratio{0.80}; // 80%+ отмен = spoof
        uint32_t                  min_events{5};          // мин событий для оценки
        std::chrono::seconds      spoof_window{60};       // окно наблюдения
        std::chrono::seconds      flag_ttl{300};          // как долго уровень под подозрением
    };

    explicit SpoofFilter(Config cfg = {}) noexcept : cfg_(cfg) {}

    // Вызывается при ADD крупной заявки
    void on_large_add(double price,
                      std::chrono::system_clock::time_point ts) noexcept {
        auto& s = stats_[round_price(price)];
        s.last_add_ts = ts;
        ++s.adds;
        evict_old(ts);
    }

    // Вызывается при DELETE крупной заявки (до исполнения)
    void on_large_cancel(double price,
                         std::chrono::system_clock::time_point ts) noexcept {
        auto& s = stats_[round_price(price)];
        ++s.cancels;
        maybe_flag(price, ts);
        evict_old(ts);
    }

    // Вызывается при частичном/полном FILL крупной заявки
    void on_large_fill(double price,
                       std::chrono::system_clock::time_point /*ts*/) noexcept {
        auto& s = stats_[round_price(price)];
        ++s.fills;
    }

    // Проверка: не стоит ли доверять этому уровню стакана?
    [[nodiscard]] bool is_spoofed(double price,
                                  std::chrono::system_clock::time_point now) const noexcept {
        const auto it = flagged_.find(round_price(price));
        if (it == flagged_.end()) return false;
        return (now - it->second) < cfg_.flag_ttl;
    }

    // Сброс в начале сессии
    void session_reset() noexcept {
        stats_.clear();
        flagged_.clear();
    }

private:
    struct LevelStats {
        uint32_t adds{};
        uint32_t cancels{};
        uint32_t fills{};
        std::chrono::system_clock::time_point last_add_ts{};
    };

    void maybe_flag(double price,
                    std::chrono::system_clock::time_point ts) noexcept {
        const auto key = round_price(price);
        const auto& s  = stats_[key];
        const uint32_t total = s.cancels + s.fills;
        if (total < cfg_.min_events) return;
        const double ratio = static_cast<double>(s.cancels) /
                             static_cast<double>(total);
        if (ratio >= cfg_.max_cancel_ratio)
            flagged_[key] = ts;
    }

    void evict_old(std::chrono::system_clock::time_point now) noexcept {
        // Удаляем записи без активности дольше spoof_window
        for (auto it = stats_.begin(); it != stats_.end(); ) {
            if ((now - it->second.last_add_ts) > cfg_.spoof_window)
                it = stats_.erase(it);
            else
                ++it;
        }
    }

    // Округление до целого тика (упрощение: double→int64 ключ)
    [[nodiscard]] static int64_t round_price(double p) noexcept {
        return static_cast<int64_t>(std::round(p * 100.0));
    }

    Config cfg_;
    std::unordered_map<int64_t, LevelStats> stats_;
    std::unordered_map<int64_t, std::chrono::system_clock::time_point> flagged_;
};

} // namespace finam::strategy
