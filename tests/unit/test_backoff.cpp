#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "core/backoff.hpp"

using namespace finam::core;
using namespace std::chrono_literals;

TEST_CASE("ExponentialBackoff: initial delay", "[backoff]") {
    ExponentialBackoff bo;
    const auto d = bo.next_delay();
    // attempt=0: base=500ms ±25%, значит [375, 625] ms
    CHECK(d >= 375ms);
    CHECK(d <= 625ms);
}

TEST_CASE("ExponentialBackoff: doubles each attempt", "[backoff]") {
    ExponentialBackoff bo;
    auto d0 = bo.next_delay().count();  // attempt 0
    auto d1 = bo.next_delay().count();  // attempt 1
    auto d2 = bo.next_delay().count();  // attempt 2
    // С джиттером точное сравнение не работает,
    // проверяем что d1 > d0 и d2 > d1 (монотонность в среднем)
    CHECK(d1 > d0);
    CHECK(d2 > d1);
}

TEST_CASE("ExponentialBackoff: caps at max_delay", "[backoff]") {
    ExponentialBackoff bo;
    // Прокручиваем 10 раз — должны упереться в cap
    for (int i = 0; i < 10; ++i) bo.next_delay();
    const auto d = bo.next_delay();
    // max_delay=30s ±25%: не превышает 30s * 1.25 = 37.5s
    CHECK(d <= 37500ms);
}

TEST_CASE("ExponentialBackoff: reset clears attempt counter", "[backoff]") {
    ExponentialBackoff bo;
    for (int i = 0; i < 5; ++i) bo.next_delay();
    CHECK(bo.attempt() == 5);
    bo.reset();
    CHECK(bo.attempt() == 0);
    // После сброса — снова начальная задержка
    const auto d = bo.next_delay();
    CHECK(d >= 375ms);
    CHECK(d <= 625ms);
}

TEST_CASE("ExponentialBackoff: wait returns false when stop=true", "[backoff]") {
    ExponentialBackoff bo;
    std::atomic<bool> stop{true};  // уже установлен
    const auto t0 = std::chrono::steady_clock::now();
    const bool result = bo.wait(stop);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK_FALSE(result);
    // Должны вернуться быстро, не спать всю задержку
    CHECK(elapsed < 200ms);
}
