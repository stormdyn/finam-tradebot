#pragma once
#include <chrono>
#include <deque>
#include <numeric>
#include <cmath>
#include "ofi_types.hpp"

namespace finam::strategy {

class TradeFlowAnalyzer {
public:
    struct Config {
        std::chrono::milliseconds tfi_window{5'000};
        std::chrono::milliseconds velocity_window{1'000};
        double   large_print_mult{4.0};
        uint32_t avg_volume_period{100};
        double   signal_threshold{0.3};

        Config() = default;
    };

    explicit TradeFlowAnalyzer(Config cfg = {}) noexcept : cfg_(cfg) {}

    TfiResult on_trade(const TradeEvent& e) noexcept {
        evict_old(e.ts);

        window_.push_back(e);
        cvd_ += e.is_buy ? e.volume : -e.volume;

        update_avg_volume(e.volume);

        return build_result(e);
    }

    void session_reset() noexcept {
        cvd_ = 0.0;
        window_.clear();
        velocity_window_.clear();
    }

    [[nodiscard]] TfiResult last() const noexcept { return last_result_; }

    [[nodiscard]] Vote vote() const noexcept {
        const double norm = normalized_tfi();
        if (norm >  cfg_.signal_threshold) return Vote::Long;
        if (norm < -cfg_.signal_threshold) return Vote::Short;
        return Vote::Neutral;
    }

    [[nodiscard]] double cvd() const noexcept { return cvd_; }

private:
    void evict_old(std::chrono::system_clock::time_point now) noexcept {
        const auto tfi_cutoff      = now - cfg_.tfi_window;
        const auto velocity_cutoff = now - cfg_.velocity_window;
        while (!window_.empty() && window_.front().ts < tfi_cutoff)
            window_.pop_front();
        while (!velocity_window_.empty() && velocity_window_.front() < velocity_cutoff)
            velocity_window_.pop_front();
        velocity_window_.push_back(now);
    }

    void update_avg_volume(double vol) noexcept {
        recent_volumes_.push_back(vol);
        if (recent_volumes_.size() > cfg_.avg_volume_period)
            recent_volumes_.pop_front();
    }

    [[nodiscard]] double normalized_tfi() const noexcept {
        double buy_vol{}, sell_vol{};
        for (const auto& t : window_) {
            if (t.is_buy) buy_vol  += t.volume;
            else          sell_vol += t.volume;
        }
        const double total = buy_vol + sell_vol;
        if (total < 1e-9) return 0.0;
        return (buy_vol - sell_vol) / total;
    }

    [[nodiscard]] double avg_volume() const noexcept {
        if (recent_volumes_.empty()) return 1.0;
        return std::accumulate(recent_volumes_.begin(),
                               recent_volumes_.end(), 0.0)
               / static_cast<double>(recent_volumes_.size());
    }

    TfiResult build_result(const TradeEvent& e) noexcept {
        TfiResult r;
        r.tfi      = normalized_tfi();
        r.cvd      = cvd_;
        r.velocity = static_cast<double>(velocity_window_.size());
        r.large_print = (e.volume > avg_volume() * cfg_.large_print_mult);
        r.ts       = e.ts;
        last_result_ = r;
        return r;
    }

    Config cfg_;
    double cvd_{0.0};
    std::deque<TradeEvent>                              window_;
    std::deque<std::chrono::system_clock::time_point>  velocity_window_;
    std::deque<double>                                  recent_volumes_;
    TfiResult last_result_{};
};

} // namespace finam::strategy
