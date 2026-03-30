#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include "ofi_types.hpp"
#include "core/interfaces.hpp"

namespace finam::strategy {

// ── SessionContext: ORB + NR7 ──────────────────────────────────────────────
//
// Логика:
//   1. NR7: если дневной диапазон (H-L) предыдущего дня — наименьший
//      за последние 7 дней → выставляем nr7_confirmed = true
//      (слабое условие — вход с 0.5x размером, не блокирует)
//
//   2. ORB: фиксируем High/Low первых orb_duration минут сессии.
//      После закрытия ORB-окна (10:00 + orb_duration):
//        - bias = Long  если last_price > orb_high
//        - bias = Short если last_price < orb_low
//        - bias = None  если внутри диапазона (флэт — не торгуем)
//
//   3. Фильтр ATR: входим в ORB только если
//      текущий дневной диапазон > atr_filter_mult * atr_N
//      (волатильный день — иначе ORB ненадёжен)
//
// Использование:
//   - Вызывать on_daily_bar() для каждого исторического D1 бара при старте
//   - Вызывать on_intraday_bar() для каждого M1 бара текущей сессии
//   - После 10:15 MSK вызывать finalize_orb() однократно
//   - Читать bias() и nr7_confirmed() перед принятием решения
//
// Поток вызовов: strategy thread (нет shared state с gRPC).

class SessionContext {
public:
    enum class Bias { Long, Short, None };

    struct Config {
        // Длительность ORB-окна (FORTS: 10:00–10:15)
        std::chrono::minutes orb_duration{15};
        // Количество дней для NR7 (Crabel: 7)
        uint8_t nr7_period{7};
        // ATR-период для фильтра волатильности
        uint8_t atr_period{14};
        // Минимальный множитель ATR для торгового дня
        // текущий (H-L) должен быть > atr_filter_mult * ATR
        double atr_filter_mult{0.8};
        // Время начала основной сессии FORTS (MSK = UTC+3)
        // Храним как минуты от полуночи UTC: 10:00 MSK = 07:00 UTC = 420 мин
        int session_open_utc_min{420};
    };

    explicit SessionContext(Config cfg = {}) noexcept : cfg_(cfg) {}

    // ── Исторические D1 бары (вызывать при инициализации, старые → новые) ──

    void on_daily_bar(const Bar& bar) noexcept {
        // Пишем в кольцевой буфер дневных диапазонов
        const double range = bar.high - bar.low;
        daily_ranges_[daily_idx_] = range;
        daily_idx_ = (daily_idx_ + 1) % kMaxDailyHistory;
        if (daily_count_ < kMaxDailyHistory) ++daily_count_;

        update_atr(range);
        update_nr7();
    }

    // ── M1/M5 бары текущей сессии ──────────────────────────────────────────

    void on_intraday_bar(const Bar& bar) noexcept {
        if (orb_finalized_) return;

        const auto bar_min = to_utc_minutes(bar.ts);
        const int orb_end  = cfg_.session_open_utc_min +
                             static_cast<int>(cfg_.orb_duration.count());

        if (bar_min < cfg_.session_open_utc_min) return; // до открытия

        // Накапливаем ORB диапазон
        if (bar_min < orb_end) {
            orb_high_ = std::max(orb_high_, bar.high);
            orb_low_  = std::min(orb_low_,  bar.low);
            current_day_high_ = std::max(current_day_high_, bar.high);
            current_day_low_  = std::min(current_day_low_,  bar.low);
        } else {
            // Первый бар после ORB-окна — финализируем
            finalize_orb(bar.close);
        }
    }

    // Вызвать явно если on_intraday_bar не перекрыл момент закрытия окна
    void finalize_orb(double current_price) noexcept {
        if (orb_finalized_) return;
        orb_finalized_ = true;

        // ATR-фильтр: не торгуем в тихие дни
        const double day_range = current_day_high_ - current_day_low_;
        if (atr_ > 1e-9 && day_range < cfg_.atr_filter_mult * atr_) {
            bias_ = Bias::None;
            return;
        }

        if (orb_high_ < -1e8) { // ORB не накоплен (нет баров)
            bias_ = Bias::None;
            return;
        }

        if (current_price > orb_high_)      bias_ = Bias::Long;
        else if (current_price < orb_low_)  bias_ = Bias::Short;
        else                                bias_ = Bias::None;
    }

    // Сброс в начале каждой новой сессии (вызывать в 10:00 MSK)
    void session_reset() noexcept {
        orb_high_         = std::numeric_limits<double>::lowest();
        orb_low_          = std::numeric_limits<double>::max();
        current_day_high_ = std::numeric_limits<double>::lowest();
        current_day_low_  = std::numeric_limits<double>::max();
        bias_             = Bias::None;
        orb_finalized_    = false;
    }

    // ── Геттеры ────────────────────────────────────────────────────────────

    [[nodiscard]] Bias   bias()          const noexcept { return bias_; }
    [[nodiscard]] bool   nr7_confirmed() const noexcept { return nr7_confirmed_; }
    [[nodiscard]] bool   orb_finalized() const noexcept { return orb_finalized_; }
    [[nodiscard]] double orb_high()      const noexcept { return orb_high_; }
    [[nodiscard]] double orb_low()       const noexcept { return orb_low_; }
    [[nodiscard]] double atr()           const noexcept { return atr_; }

    // Проверка: совместим ли vote с текущим bias?
    // nr7_confirmed влияет на size_multiplier, не на блокировку
    [[nodiscard]] bool allows_long()  const noexcept { return bias_ == Bias::Long;  }
    [[nodiscard]] bool allows_short() const noexcept { return bias_ == Bias::Short; }

    // Множитель размера позиции: 1.0 если NR7, 0.5 иначе
    [[nodiscard]] double size_multiplier() const noexcept {
        return nr7_confirmed_ ? 1.0 : 0.5;
    }

private:
    static constexpr int kMaxDailyHistory = 30; // хватит для NR7+ATR14

    void update_atr(double range) noexcept {
        // Простой скользящий ATR (Wilder's смягчение)
        if (atr_ < 1e-9) {
            atr_ = range;
            return;
        }
        const double k = 1.0 / static_cast<double>(cfg_.atr_period);
        atr_ = atr_ * (1.0 - k) + range * k;
    }

    void update_nr7() noexcept {
        if (daily_count_ < cfg_.nr7_period) {
            nr7_confirmed_ = false;
            return;
        }
        // Последний записанный диапазон — предыдущий день
        // (текущий день ещё не закрыт)
        const int last_idx = (daily_idx_ - 1 + kMaxDailyHistory) % kMaxDailyHistory;
        const double last_range = daily_ranges_[last_idx];

        // Проверяем что он минимален среди последних nr7_period дней
        for (int i = 1; i < cfg_.nr7_period; ++i) {
            const int idx = (daily_idx_ - 1 - i + kMaxDailyHistory) % kMaxDailyHistory;
            if (daily_ranges_[idx] <= last_range) {
                nr7_confirmed_ = false;
                return;
            }
        }
        nr7_confirmed_ = true;
    }

    [[nodiscard]] static int to_utc_minutes(
        std::chrono::system_clock::time_point ts) noexcept
    {
        const auto tt = std::chrono::system_clock::to_time_t(ts);
        // Минуты с полуночи UTC
        return static_cast<int>((tt % 86400) / 60);
    }

    Config cfg_;

    // NR7 + ATR: кольцевой буфер дневных диапазонов
    std::array<double, kMaxDailyHistory> daily_ranges_{};
    int     daily_idx_{0};
    int     daily_count_{0};
    double  atr_{0.0};
    bool    nr7_confirmed_{false};

    // ORB текущей сессии
    double  orb_high_{std::numeric_limits<double>::lowest()};
    double  orb_low_ {std::numeric_limits<double>::max()};
    double  current_day_high_{std::numeric_limits<double>::lowest()};
    double  current_day_low_ {std::numeric_limits<double>::max()};
    Bias    bias_{Bias::None};
    bool    orb_finalized_{false};
};

} // namespace finam::strategy