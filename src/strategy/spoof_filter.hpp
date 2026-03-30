#pragma once
#include <chrono>
#include <unordered_map>
#include <cmath>
#include "ofi_types.hpp"

namespace finam::strategy {

struct SpoofFilterConfig {
    double               min_large_qty{10.0};
    double               max_cancel_ratio{0.80};
    uint32_t             min_events{5};
    std::chrono::seconds spoof_window{60};
    std::chrono::seconds flag_ttl{300};
};

class SpoofFilter {
public:
    using Config = SpoofFilterConfig;

    explicit SpoofFilter(Config cfg = {}) noexcept : cfg_(cfg) {}

    void on_large_add(double price,
                      std::chrono::system_clock::time_point ts) noexcept {
        auto& s = stats_[round_price(price)];
        s.last_add_ts = ts;
        ++s.adds;
        evict_old(ts);
    }

    void on_large_cancel(double price,
                         std::chrono::system_clock::time_point ts) noexcept {
        auto& s = stats_[round_price(price)];
        ++s.cancels;
        maybe_flag(price, ts);
        evict_old(ts);
    }

    void on_large_fill(double price,
                       std::chrono::system_clock::time_point /*ts*/) noexcept {
        auto& s = stats_[round_price(price)];
        ++s.fills;
    }

    [[nodiscard]] bool is_spoofed(double price,
                                  std::chrono::system_clock::time_point now) const noexcept {
        const auto it = flagged_.find(round_price(price));
        if (it == flagged_.end()) return false;
        return (now - it->second) < cfg_.flag_ttl;
    }

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
        for (auto it = stats_.begin(); it != stats_.end(); ) {
            if ((now - it->second.last_add_ts) > cfg_.spoof_window)
                it = stats_.erase(it);
            else
                ++it;
        }
    }

    [[nodiscard]] static int64_t round_price(double p) noexcept {
        return static_cast<int64_t>(std::round(p * 100.0));
    }

    Config cfg_;
    std::unordered_map<int64_t, LevelStats> stats_;
    std::unordered_map<int64_t, std::chrono::system_clock::time_point> flagged_;
};

} // namespace finam::strategy
