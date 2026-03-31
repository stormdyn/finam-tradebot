#pragma once
#include <chrono>
#include <string>
#include "interfaces.hpp"

namespace finam::core {

// ── nearest_contract ────────────────────────────────────────────────────────────────
//
// Возвращает ближайший Symbol для заданного тикера ("Si", "RTS", "GOLD").
// Квартальные фьючерсы FORTS экспирируют в марте, июне, сентябре, декабре.
// Если сегодня >= third_friday(expiry_month) - rollover_days,
// используем следующий квартальный месяц.
//
// Пример: 31 марта 2026 → "Si-6.26@FORTS" (мартовский Si-3.26 уже в зоне ролловера)

// Третья пятница месяца (UTC, 1-based day).
// FIX: использует C++20 std::chrono::year_month_day вместо mktime(),
//      чтобы избежать зависимости от локальной таймзоны системы.
[[nodiscard]] inline int third_friday(int year, int month) noexcept {
    using namespace std::chrono;
    const auto ymd = year_month_day{
        std::chrono::year(year),
        std::chrono::month(static_cast<unsigned>(month)),
        std::chrono::day(1)
    };
    const auto sys_day = static_cast<sys_days>(ymd);
    const weekday wd{sys_day};
    // weekday::c_encoding(): 0=Sun, 1=Mon, ..., 5=Fri, 6=Sat
    const unsigned wdi = wd.c_encoding();  // 0-based, 0=Sun
    const unsigned days_to_first_fri = (wdi <= 5) ? (5 - wdi) : (12 - wdi);
    return 1 + static_cast<int>(days_to_first_fri) + 14;
}

// Квартальные месяцы FORTS
[[nodiscard]] inline bool is_quarterly(int month) noexcept {
    return month == 3 || month == 6 || month == 9 || month == 12;
}

// FIX: прямое вычисление следующего квартального месяца без цикла.
// Старая реализация через while(!is_quarterly) + month+=3 зацикливалась
// при нон-квартальных месяцах (Jan/Feb/Apr/May/Jul/Aug/Oct/Nov).
[[nodiscard]] inline int next_quarterly_month(int month) noexcept {
    if (month < 3)  return 3;
    if (month < 6)  return 6;
    if (month < 9)  return 9;
    if (month < 12) return 12;
    return 3;  // январь следующего года → март
}

inline void next_quarterly(int& month, int& year) noexcept {
    const int next = next_quarterly_month(month);
    if (next <= month) ++year;  // переход через год (декабрь → март)
    month = next;
}

[[nodiscard]] inline Symbol nearest_contract(
    std::string_view ticker,
    int rollover_days = 5,
    std::chrono::system_clock::time_point now
        = std::chrono::system_clock::now()) noexcept
{
    using namespace std::chrono;
    const auto dp  = floor<days>(now);
    const year_month_day ymd{dp};

    int cur_year  = static_cast<int>(ymd.year());
    int cur_month = static_cast<int>(static_cast<unsigned>(ymd.month()));
    int cur_day   = static_cast<int>(static_cast<unsigned>(ymd.day()));

    int exp_month = cur_month;
    int exp_year  = cur_year;

    // Доводим до ближайшего квартального (O(1) — без цикла)
    if (!is_quarterly(exp_month))
        next_quarterly(exp_month, exp_year);

    // Если уже в зоне ролловера — берём следующий квартал
    const int tf = third_friday(exp_year, exp_month);
    if (cur_year == exp_year && cur_month == exp_month &&
        cur_day >= tf - rollover_days)
    {
        next_quarterly(exp_month, exp_year);
    }

    // Формат: Si-6.26
    const int yy = exp_year % 100;
    const std::string code = std::string(ticker) + "-"
        + std::to_string(exp_month) + "."
        + (yy < 10 ? "0" : "") + std::to_string(yy);

    return Symbol{code, "FORTS"};
}

// Возвращает день третьей пятницы для контракта по его символу
[[nodiscard]] inline int expiry_day(const Symbol& sym) noexcept {
    const auto& code = sym.security_code;
    const auto  dash = code.find('-');
    const auto  dot  = code.find('.');
    if (dash == std::string::npos || dot == std::string::npos) return 0;
    try {
        const int month = std::stoi(code.substr(dash + 1, dot - dash - 1));
        const int year  = 2000 + std::stoi(code.substr(dot + 1));
        return third_friday(year, month);
    } catch (...) { return 0; }
}

} // namespace finam::core
