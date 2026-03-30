#pragma once
#include <chrono>
#include <string>
#include <ctime>
#include "interfaces.hpp"

namespace finam::core {

// ── nearest_contract ────────────────────────────────────────────────────────────────
//
// Возвращает наиближайший Symbol для заданного тикера ("Si", "RTS", "GOLD").
// Квартальные фьючерсы FORTS экспирируют в марте, июне, сентябре, декабре.
// Если сегодня >= third_friday(expiry_month) - rollover_days,
// используем следующий квартальный месяц.
//
// Пример: 30 марта 2026 → "Si-6.26@FORTS" (мартовский Si-3.26 уже в зоне ролловера)

// Третья пятница месяца (UTC, 1-based day)
[[nodiscard]] inline int third_friday(int year, int month) noexcept {
    // День недели 1-го числа
    struct tm t{};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = 1;
    mktime(&t);  // заполняет tm_wday
    // tm_wday: 0=Sun,1=Mon,...,5=Fri
    const int wday = t.tm_wday;
    const int days_to_first_fri = (wday <= 5) ? (5 - wday) : (12 - wday);
    return 1 + days_to_first_fri + 14;  // третья пятница
}

// Квартальные месяцы FORTS
[[nodiscard]] inline bool is_quarterly(int month) noexcept {
    return month == 3 || month == 6 || month == 9 || month == 12;
}

// Следующий квартальный месяц (результат в [month, year] by ref)
inline void next_quarterly(int& month, int& year) noexcept {
    // шагаем по кварталам: 3→6→9→12→3 следующего года
    month += 3;
    if (month > 12) { month -= 12; ++year; }
}

[[nodiscard]] inline Symbol nearest_contract(
    std::string_view ticker,
    int rollover_days = 5,
    std::chrono::system_clock::time_point now
        = std::chrono::system_clock::now()) noexcept
{
    const auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm t{};
    gmtime_r(&tt, &t);

    int cur_year  = t.tm_year + 1900;
    int cur_month = t.tm_mon  + 1;
    int cur_day   = t.tm_mday;

    // Находим текущий квартальный месяц (текущий или следующий)
    int exp_month = cur_month;
    int exp_year  = cur_year;
    // Доводим до ближайшего квартального
    while (!is_quarterly(exp_month)) next_quarterly(exp_month, exp_year);

    // Если уже в зоне ролловера (cur_day >= third_friday - rollover_days) — берём следующий
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

// Инфо: third_friday текущего контракта
[[nodiscard]] inline int expiry_day(
    const Symbol& sym) noexcept
{
    const auto& code  = sym.security_code;
    const auto  dash  = code.find('-');
    const auto  dot   = code.find('.');
    if (dash == std::string::npos || dot == std::string::npos) return 0;
    try {
        const int month = std::stoi(code.substr(dash + 1, dot - dash - 1));
        const int year  = 2000 + std::stoi(code.substr(dot + 1));
        return third_friday(year, month);
    } catch (...) { return 0; }
}

} // namespace finam::core
