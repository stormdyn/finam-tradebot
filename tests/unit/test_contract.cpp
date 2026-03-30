#include <catch2/catch_test_macros.hpp>
#include "core/contract.hpp"

using namespace finam::core;
using namespace finam;

TEST_CASE("third_friday: March 2026", "[contract]") {
    // Январь 2026: 1-е = четверг, первая пятница = 2, третья = 16
    // Март 2026: 1-е = воскресенье, первая пятница = 6, третья = 20
    CHECK(third_friday(2026, 3) == 20);
}

TEST_CASE("third_friday: June 2026", "[contract]") {
    // Июнь 2026: 1-е = понедельник, первая пятница = 5, третья = 19
    CHECK(third_friday(2026, 6) == 19);
}

TEST_CASE("nearest_contract: before rollover window", "[contract]") {
    // 10 марта 2026: third_friday=20, 10 < 20-5=15 → ещё Si-3.26
    using namespace std::chrono;
    const auto t = system_clock::from_time_t([]{
        struct tm tm{};
        tm.tm_year=126; tm.tm_mon=2; tm.tm_mday=10;
        return timegm(&tm);
    }());
    const auto sym = nearest_contract("Si", 5, t);
    CHECK(sym.security_code == "Si-3.26");
}

TEST_CASE("nearest_contract: inside rollover window", "[contract]") {
    // 16 марта 2026: 16 >= 20-5=15 → уже Si-6.26
    using namespace std::chrono;
    const auto t = system_clock::from_time_t([]{
        struct tm tm{};
        tm.tm_year=126; tm.tm_mon=2; tm.tm_mday=16;
        return timegm(&tm);
    }());
    const auto sym = nearest_contract("Si", 5, t);
    CHECK(sym.security_code == "Si-6.26");
}

TEST_CASE("nearest_contract: 30 March 2026 = Si-6.26", "[contract]") {
    // Сегодняшний день из задания системы
    using namespace std::chrono;
    const auto t = system_clock::from_time_t([]{
        struct tm tm{};
        tm.tm_year=126; tm.tm_mon=2; tm.tm_mday=30;
        return timegm(&tm);
    }());
    const auto sym = nearest_contract("Si", 5, t);
    CHECK(sym.security_code == "Si-6.26");
}

TEST_CASE("nearest_contract: December rollover to March next year", "[contract]") {
    // 12 декабря 2026: third_friday(2026,12)=18, 12 < 18-5=13 → ещё Si-12.26
    using namespace std::chrono;
    const auto t = system_clock::from_time_t([]{
        struct tm tm{};
        tm.tm_year=126; tm.tm_mon=11; tm.tm_mday=12;
        return timegm(&tm);
    }());
    const auto sym = nearest_contract("Si", 5, t);
    CHECK(sym.security_code == "Si-12.26");
}
