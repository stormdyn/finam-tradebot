#pragma once
#include <chrono>
#include <string>
#include "interfaces.hpp"

namespace finam::core {

// ── nearest_contract ──────────────────────────────────────────────────────────
//
// Реальный формат Finam Trade API (подтверждён через GetAsset):
//   symbol = "{ROOT}{LETTER}{YY}@RTSX"
//   MIC    = RTSX  («МОСКОВСКАЯ БИРЖА — СРОЧНЫЙ РЫНОК»)
//   LETTER = H(март) M(июнь) U(сентябрь) Z(декабрь)
//   YY     = последние 2 цифры года
//
// Примеры: SiM6@RTSX, RIM6@RTSX, GDM6@RTSX, MXM6@RTSX, BRM6@RTSX
// СТАРЫЙ формат Si-6.26@FORTS — НЕРАБОЧИЙ, API его не знает!

// Третья пятница месяца (C++20 chrono)
[[nodiscard]] inline int third_friday(int year, int month) noexcept {
    using namespace std::chrono;
    const auto ymd = year_month_day{
        std::chrono::year(year),
        std::chrono::month(static_cast<unsigned>(month)),
        std::chrono::day(1)
    };
    const auto sys_day = static_cast<sys_days>(ymd);
    const weekday wd{sys_day};
    const unsigned wdi = wd.c_encoding();  // 0=Sun, 5=Fri
    const unsigned days_to_first_fri = (wdi <= 5) ? (5 - wdi) : (12 - wdi);
    return 1 + static_cast<int>(days_to_first_fri) + 14;
}

[[nodiscard]] inline bool is_quarterly(int month) noexcept {
    return month == 3 || month == 6 || month == 9 || month == 12;
}

[[nodiscard]] inline int next_quarterly_month(int month) noexcept {
    if (month < 3)  return 3;
    if (month < 6)  return 6;
    if (month < 9)  return 9;
    if (month < 12) return 12;
    return 3;
}

inline void next_quarterly(int& month, int& year) noexcept {
    const int next = next_quarterly_month(month);
    if (next <= month) ++year;
    month = next;
}

// Буква квартала FORTS: 3→H, 6→M, 9→U, 12→Z
[[nodiscard]] inline char quarter_letter(int month) noexcept {
    switch (month) {
        case 3:  return 'H';
        case 6:  return 'M';
        case 9:  return 'U';
        case 12: return 'Z';
        default: return 'X';  // не должно дойти
    }
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

    if (!is_quarterly(exp_month))
        next_quarterly(exp_month, exp_year);

    const int tf = third_friday(exp_year, exp_month);
    if (cur_year == exp_year && cur_month == exp_month &&
        cur_day >= tf - rollover_days)
    {
        next_quarterly(exp_month, exp_year);
    }

    // Формат: {ROOT}{LETTER}{YY}  напр.: SiM6, RIM6, GDM6
    const int  yy     = exp_year % 100;
    const char letter = quarter_letter(exp_month);
    const std::string code = std::string(ticker)
        + letter
        + (yy < 10 ? "0" : "") + std::to_string(yy);

    return Symbol{code, "RTSX"};
}

// День экспирации по символу ({ROOT}{LETTER}{YY}@RTSX)
[[nodiscard]] inline int expiry_day(const Symbol& sym) noexcept {
    const auto& code = sym.security_code;
    if (code.size() < 3) return 0;
    const char letter = code[code.size() - 3];  // предпоследние 3 символа = LETTER + YY
    // YY — последние 2 символа
    try {
        const int yy   = std::stoi(code.substr(code.size() - 2));
        const int year = 2000 + yy;
        int month = 0;
        switch (letter) {
            case 'H': month = 3;  break;
            case 'M': month = 6;  break;
            case 'U': month = 9;  break;
            case 'Z': month = 12; break;
            default: return 0;
        }
        return third_friday(year, month);
    } catch (...) { return 0; }
}

} // namespace finam::core
