#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace finam::strategy {

inline constexpr int kBookLevels = 5;

// ── Результат MLOFI ──────────────────────────────────────────────────────────
// score > 0  → давление покупателей
// score < 0  → давление продавцов
// abs(score) → сила сигнала
struct MlofiResult {
    double score{};                          // взвешенная сумма по уровням
    std::array<double, kBookLevels> levels{}; // OFI по каждому уровню (debug)
    std::chrono::system_clock::time_point ts;
};

// ── Результат Trade Flow Imbalance ───────────────────────────────────────────
struct TfiResult {
    double tfi{};      // buy_vol - sell_vol за окно
    double cvd{};      // накопленный с начала сессии
    double velocity{}; // сделок в секунду за последние N мс
    bool   large_print{false}; // текущая сделка аномально большая
    std::chrono::system_clock::time_point ts;
};

// ── Вотум для системы голосования ────────────────────────────────────────────
enum class Vote : int8_t { Long = 1, Short = -1, Neutral = 0 };

struct OfiVote {
    Vote   mlofi{Vote::Neutral};
    Vote   tfi{Vote::Neutral};
    int8_t confluence{}; // сумма: +2 strong long, -2 strong short, ±1 weak

    [[nodiscard]] bool is_strong() const noexcept {
        return confluence == 2 || confluence == -2;
    }
    [[nodiscard]] bool is_long()  const noexcept { return confluence > 0; }
    [[nodiscard]] bool is_short() const noexcept { return confluence < 0; }
};

// ── Событие из стакана (дельта уровня) ───────────────────────────────────────
// Получается при разборе SubscribeOrderBook стрима
struct BookLevelEvent {
    int     level{};           // 0-based, 0 = лучший уровень
    double  price{};
    double  old_bid_size{};
    double  new_bid_size{};
    double  old_ask_size{};
    double  new_ask_size{};
    std::chrono::system_clock::time_point ts;
};

// ── Событие исполненной сделки ────────────────────────────────────────────────
struct TradeEvent {
    double  price{};
    double  volume{};
    bool    is_buy{}; // true = buyer-initiated (hit ask)
    std::chrono::system_clock::time_point ts;
};

} // namespace finam::strategy
