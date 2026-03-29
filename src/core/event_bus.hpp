#pragma once
// Lightweight SPSC event bus.
// Producer: MarketDataClient (gRPC thread)
// Consumer: Strategy engine (main loop)
//
// Uses a fixed-size ring buffer; no heap allocation in hot path.
// If the ring is full, the oldest event is overwritten (lossy — acceptable
// for quotes, use larger capacity if needed).

#include <atomic>
#include <array>
#include <optional>
#include <cstddef>
#include <variant>
#include "interfaces.hpp"

namespace finam {

// Event variant — extend when adding new event types
using Event = std::variant<Quote, Bar, OrderUpdate>;

template<std::size_t Capacity>
class SPSCEventBus {
static_assert((Capacity & (Capacity - 1)) == 0,
              "Capacity must be a power of 2");
public:
    // Producer side — called from gRPC thread
    void push(Event ev) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        buffer_[head & kMask] = std::move(ev);
        head_.store(head + 1, std::memory_order_release);
    }

    // Consumer side — called from strategy thread
    [[nodiscard]] std::optional<Event> pop() noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return std::nullopt;
        auto ev = std::move(buffer_[tail & kMask]);
        tail_.store(tail + 1, std::memory_order_release);
        return ev;
    }

    [[nodiscard]] bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::array<Event, Capacity>          buffer_{};
};

// Default bus: 4096 events (~256 KB for Quote-heavy workloads)
using DefaultEventBus = SPSCEventBus<4096>;

} // namespace finam
