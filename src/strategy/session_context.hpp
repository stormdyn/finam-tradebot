#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include "ofi_types.hpp"
#include "core/interfaces.hpp"

namespace finam::strategy {

class SessionContext {
public:
    enum class Bias { Long, Short, None };

    struct Config {
        std::chrono::minutes orb_duration{15};
        uint8_t nr7_period{7};
        uint8_t atr_period{14};
        double  atr_filter_mult{0.8};
        int     session_open_utc_min{420};
    };

    explicit SessionContext(Config cfg) noexcept : cfg_(cfg) {}
    SessionContext() noexcept : SessionContext(Config{}) {}

    void on_daily_bar(const Bar& bar) noexcept {
        const double range = bar.high - bar.low;
        daily_ranges_[daily_idx_] = range;
        daily_idx_ = (daily_idx_ + 1) % kMaxDailyHistory;
        if (daily_count_ < kMaxDailyHistory) ++daily_count_;

        update_atr(range);
        update_nr7();
    }

    void on_intraday_bar(const Bar& bar) noexcept {
        if (orb_finalized_) return;

        const auto bar_min = to_utc_minutes(bar.ts);
        const int orb_end  = cfg_.session_open_utc_min +
                             static_cast<int>(cfg_.orb_duration.count());

        if (bar_min < cfg_.session_open_utc_min) return;

        if (bar_min < orb_end) {
            orb_high_ = std::max(orb_high_, bar.high);
            orb_low_  = std::min(orb_low_,  bar.low);
            current_day_high_ = std::max(current_day_high_, bar.high);
            current_day_low_  = std::min(current_day_low_,  bar.low);
        } else {
            finalize_orb(bar.close);
        }
    }

    void finalize_orb(double current_price) noexcept {
        if (orb_finalized_) return;
        orb_finalized_ = true;

        const double day_range = current_day_high_ - current_day_low_;
        if (atr_ > 1e-9 && day_range < cfg_.atr_filter_mult * atr_) {
            bias_ = Bias::None;
            return;
        }

        if (orb_high_ < -1e8) {
            bias_ = Bias::None;
            return;
        }

        if (current_price > orb_high_)      bias_ = Bias::Long;
        else if (current_price < orb_low_)  bias_ = Bias::Short;
        else                                bias_ = Bias::None;
    }

    void session_reset() noexcept {
        orb_high_         = std::numeric_limits<double>::lowest();
        orb_low_          = std::numeric_limits<double>::max();
        current_day_high_ = std::numeric_limits<double>::lowest();
        current_day_low_  = std::numeric_limits<double>::max();
        bias_             = Bias::None;
        orb_finalized_    = false;
    }

    [[nodiscard]] Bias   bias()          const noexcept { return bias_; }
    [[nodiscard]] bool   nr7_confirmed() const noexcept { return nr7_confirmed_; }
    [[nodiscard]] bool   orb_finalized() const noexcept { return orb_finalized_; }
    [[nodiscard]] double orb_high()      const noexcept { return orb_high_; }
    [[nodiscard]] double orb_low()       const noexcept { return orb_low_; }
    [[nodiscard]] double atr()           const noexcept { return atr_; }

    [[nodiscard]] bool allows_long()  const noexcept { return bias_ == Bias::Long;  }
    [[nodiscard]] bool allows_short() const noexcept { return bias_ == Bias::Short; }

    [[nodiscard]] double size_multiplier() const noexcept {
        return nr7_confirmed_ ? 1.0 : 0.5;
    }

private:
    static constexpr int kMaxDailyHistory = 30;

    void update_atr(double range) noexcept {
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
        const int last_idx = (daily_idx_ - 1 + kMaxDailyHistory) % kMaxDailyHistory;
        const double last_range = daily_ranges_[last_idx];

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
        return static_cast<int>((tt % 86400) / 60);
    }

    Config cfg_;

    std::array<double, kMaxDailyHistory> daily_ranges_{};
    int     daily_idx_{0};
    int     daily_count_{0};
    double  atr_{0.0};
    bool    nr7_confirmed_{false};

    double  orb_high_{std::numeric_limits<double>::lowest()};
    double  orb_low_ {std::numeric_limits<double>::max()};
    double  current_day_high_{std::numeric_limits<double>::lowest()};
    double  current_day_low_ {std::numeric_limits<double>::max()};
    Bias    bias_{Bias::None};
    bool    orb_finalized_{false};
};

} // namespace finam::strategy
