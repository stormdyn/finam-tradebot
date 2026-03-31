#pragma once
#include <atomic>
#include <array>
#include <optional>
#include <variant>
#include "core/interfaces.hpp"

namespace finam {

// ── Event = всё что может прийти с рынка или от брокера ──────────────────────
using MarketEvent = std::variant<Quote, Bar, OrderBook, OrderUpdate>;

// ── SPSC lock-free кольцевой буфер ───────────────────────────────────────────
// Producer: MD-поток (gRPC stream reader)
// Consumer: Strategy-поток (event loop)
//
// Capacity должен быть степенью двойки — маска вместо модуля.
// Cacheline padding между head/tail/buffer — устраняет false sharing.
//
template<std::size_t Capacity>
    requires (Capacity >= 2 && (Capacity & (Capacity - 1)) == 0)  // pow2
class SPSCQueue {
public:
    // Возвращает false если буфер полон (producer dropped event — логируй!)
    bool push(MarketEvent event) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1) & kMask;

        if (next == tail_.load(std::memory_order_acquire))
            return false;  // full

        buffer_[head] = std::move(event);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Возвращает nullopt если буфер пуст
    [[nodiscard]] std::optional<MarketEvent> pop() noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire))
            return std::nullopt;  // empty

        auto event = std::move(buffer_[tail]);
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return event;
    }

    [[nodiscard]] bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire)
            == head_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // FIX: добавлен alignas(64) к buffer_ — устраняет false sharing с tail_.
    // FIX: добавлен {} инициализатор — безопасно при добавлении non-trivial типов в variant.
    alignas(64) std::atomic<std::size_t>      head_{0};
    alignas(64) std::atomic<std::size_t>      tail_{0};
    alignas(64) std::array<MarketEvent, Capacity> buffer_{};
};

// ── Конкретный тип шины для бота ──────────────────────────────────────────────
// 1024 событий = ~40KB при sizeof(MarketEvent)≈40 байт — влезает в L2
using EventBus = SPSCQueue<1024>;

} // namespace finam
