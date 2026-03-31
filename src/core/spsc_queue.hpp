#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>

namespace finam::core {

// Lock-free SPSC (Single Producer Single Consumer) очередь.
//
// Гарантии:
//   - push() вызывается строго из одного потока (producer)
//   - pop()  вызывается строго из одного потока (consumer)
//   - Никаких мьютексов, никакого false sharing (разные cache lines)
//   - Если очередь полна — push() возвращает false (не блокирует)
//
// Использование в боте:
//   Producer: gRPC MD callback thread  → push BookLevelEvent / TradeEvent
//   Consumer: strategy thread          → pop и передать в ConfluenceStrategy
//
// Размер N должен быть степенью двойки для эффективного mask-based wrap.
// Рекомендуется N = 1024 для стакана (burst ~200 событий/сек на Si).

template<typename T, std::size_t N>
    requires (N >= 2 && (N & (N - 1)) == 0)  // N — степень двойки
class SpscQueue {
public:
    // Возвращает false если очередь полна (без блокировки)
    bool push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;

        if (next == tail_.load(std::memory_order_acquire))
            return false; // полна

        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool push(T&& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;

        if (next == tail_.load(std::memory_order_acquire))
            return false;

        buffer_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Возвращает std::nullopt если очередь пуста
    std::optional<T> pop() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire))
            return std::nullopt; // пуста

        T item = std::move(buffer_[tail]);
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & kMask;
    }

private:
    static constexpr std::size_t kMask = N - 1;

    // Разделяем head_, tail_ и buffer_ на разные cache lines.
    // FIX: добавлен alignas(64) к buffer_ — без него buffer_[0] лежал
    //      на той же cacheline что и tail_, создавая false sharing.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::array<T, N>         buffer_{};
};

} // namespace finam::core
