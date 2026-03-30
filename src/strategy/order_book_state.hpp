#pragma once
#include <array>
#include <cmath>
#include <numeric>
#include <deque>
#include "ofi_types.hpp"

namespace finam::strategy {

class OrderBookState {
public:
    struct Config {
        double level_decay{0.5};
        double time_decay_per_sec{0.9};
        double signal_threshold{3.0};
    };

    // Явный дефолтный конструктор вынесен за пределы Config — GCC
    // не может обработать default member initializers внутреннего
    // класса до окончания outer class, если Config используется в default argument.
    explicit OrderBookState(Config cfg) noexcept
        : cfg_(cfg)
    {
        precompute_weights();
    }

    OrderBookState() noexcept : OrderBookState(Config{}) {}

    MlofiResult on_book_event(const BookLevelEvent& e) noexcept {
        if (e.level < 0 || e.level >= kBookLevels) return last_result_;

        apply_time_decay(e.ts);

        const double bid_delta = e.new_bid_size - e.old_bid_size;
        const double ask_delta = e.new_ask_size - e.old_ask_size;
        const double ofi_delta = bid_delta - ask_delta;

        level_scores_[e.level] += ofi_delta;
        score_ += weights_[e.level] * ofi_delta;

        last_result_ = build_result(e.ts);
        return last_result_;
    }

    MlofiResult decay_tick(std::chrono::system_clock::time_point now) noexcept {
        apply_time_decay(now);
        last_result_.ts = now;
        return last_result_;
    }

    [[nodiscard]] MlofiResult last() const noexcept { return last_result_; }

    [[nodiscard]] Vote vote() const noexcept {
        if (score_ >  cfg_.signal_threshold) return Vote::Long;
        if (score_ < -cfg_.signal_threshold) return Vote::Short;
        return Vote::Neutral;
    }

    void reset() noexcept {
        score_ = 0.0;
        level_scores_.fill(0.0);
        last_event_ts_ = {};
        last_result_   = {};
    }

private:
    void precompute_weights() noexcept {
        for (int k = 0; k < kBookLevels; ++k)
            weights_[k] = std::exp(-cfg_.level_decay * static_cast<double>(k));
        const double sum = std::accumulate(weights_.begin(), weights_.end(), 0.0);
        for (auto& w : weights_) w /= sum;
    }

    void apply_time_decay(std::chrono::system_clock::time_point now) noexcept {
        if (last_event_ts_ == std::chrono::system_clock::time_point{}) {
            last_event_ts_ = now;
            return;
        }
        const double dt_sec = std::chrono::duration<double>(
            now - last_event_ts_).count();
        if (dt_sec <= 0.0) return;

        const double decay = std::pow(cfg_.time_decay_per_sec, dt_sec);
        score_ *= decay;
        for (auto& ls : level_scores_) ls *= decay;
        last_event_ts_ = now;
    }

    MlofiResult build_result(std::chrono::system_clock::time_point ts) const noexcept {
        MlofiResult r;
        r.score = score_;
        r.ts    = ts;
        for (int k = 0; k < kBookLevels; ++k)
            r.levels[k] = level_scores_[k];
        return r;
    }

    Config cfg_;
    std::array<double, kBookLevels>  weights_{};
    std::array<double, kBookLevels>  level_scores_{};
    double score_{0.0};
    std::chrono::system_clock::time_point last_event_ts_{};
    MlofiResult last_result_{};
};

} // namespace finam::strategy
