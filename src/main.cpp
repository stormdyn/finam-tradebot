#include <cstdlib>
#include <spdlog/spdlog.h>
#include "auth/token_manager.hpp"

int main() {
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::info("finam-tradebot starting");

    // ── Секрет из env — никогда не хардкодить ────────────────────────────────
    const char* secret = std::getenv("FINAM_SECRET_TOKEN");
    if (!secret) {
        spdlog::critical("FINAM_SECRET_TOKEN env variable not set");
        return 1;
    }

    // ── Auth ──────────────────────────────────────────────────────────────────
    auto token_mgr = std::make_shared<finam::auth::TokenManager>(
        finam::auth::Config{
            .endpoint = "api.finam.ru:443",
            .secret   = secret,
        }
    );

    if (auto r = token_mgr->init(); !r) {
        spdlog::critical("Auth failed: {}", r.error().message);
        return 1;
    }

    spdlog::info("Primary account: {}", token_mgr->primary_account_id());

    // ── TODO (следующие итерации) ─────────────────────────────────────────────
    // auto bus      = std::make_shared<finam::EventBus>();
    // auto md       = std::make_shared<finam::MarketDataClient>(token_mgr, bus);
    // auto risk     = std::make_shared<finam::RiskManager>(token_mgr);
    // auto executor = std::make_shared<finam::OrderExecutor>(token_mgr);
    // auto strategy = std::make_shared<finam::TrendFollowing>(risk, executor);
    //
    // md->subscribe(finam::Symbol{"Si", "FUT"});
    //
    // while (running) {
    //     while (auto ev = bus->pop())
    //         std::visit([&](auto&& e){ strategy->dispatch(e); }, *ev);
    // }

    token_mgr->shutdown();
    spdlog::info("finam-tradebot stopped");
    return 0;
}