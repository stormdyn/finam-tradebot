#pragma once
#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

namespace finam::core {

// ── ExponentialBackoff ──────────────────────────────────────────────────────────────
//
// Использование:
//   ExponentialBackoff bo;
//   while (!stop) {
//       connect_and_run();     // блокируется пока работает
//       bo.wait(stop);         // ждём перед реконнектом
//   }
//   // Успешное соединение — сбросить задержку:
//   bo.reset();
//
// Задержка: base * 2^attempt + jitter, зажата [0, cap].
// Джиттер: ±25% от текущего delay — избегаем reconnect storm.
// Прерывается: если stop стал true во время сна.

struct BackoffConfig {
    std::chrono::milliseconds base{500};    // начальная задержка
    std::chrono::milliseconds cap{30'000};  // максимальная задержка (30с)
    std::chrono::milliseconds tick{50};     // гранулярность прерывания
    double jitter_pct{0.25};                // ±25% джиттер
};

class ExponentialBackoff {
public:
    explicit ExponentialBackoff(BackoffConfig cfg = {}) noexcept
        : cfg_(cfg)
        , rng_(std::random_device{}())
    {}

    // Ждём с проверкой stop каждые tick мс.
    // Возвращает: true — надо реконнектиться, false — остановка
    template<typename StopFlag>
    [[nodiscard]] bool wait(const StopFlag& stop) noexcept {
        const auto delay = next_delay();
        auto remaining   = delay;
        while (remaining > std::chrono::milliseconds::zero()) {
            if (stop.load(std::memory_order_acquire)) return false;
            const auto sleep = std::min(remaining, cfg_.tick);
            std::this_thread::sleep_for(sleep);
            remaining -= sleep;
        }
        ++attempt_;
        return true;
    }

    void reset() noexcept { attempt_ = 0; }

    [[nodiscard]] int attempt() const noexcept { return attempt_; }

private:
    [[nodiscard]] std::chrono::milliseconds next_delay() noexcept {
        // base * 2^attempt, зажато до cap
        const double raw = static_cast<double>(cfg_.base.count())
                         * (1LL << std::min(attempt_, 6));  // max 64x
        const double capped = std::min(raw,
            static_cast<double>(cfg_.cap.count()));

        // Джиттер: равномерно в [capped*(1-jitter), capped*(1+jitter)]
        std::uniform_real_distribution<double> dist(
            capped * (1.0 - cfg_.jitter_pct),
            capped * (1.0 + cfg_.jitter_pct));
        return std::chrono::milliseconds{static_cast<int64_t>(dist(rng_))};
    }

    BackoffConfig          cfg_;
    std::mt19937           rng_;
    int                    attempt_{0};
};

} // namespace finam::core
