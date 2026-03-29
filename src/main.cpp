#include <cstdlib>
#include <spdlog/spdlog.h>
#include "auth/token_manager.hpp"

int main() {
    spdlog::set_level(spdlog::level::debug);
    spdlog::info("finam-tradebot starting");

    const char* secret = std::getenv("FINAM_SECRET_TOKEN");
    if (!secret) {
        spdlog::critical("FINAM_SECRET_TOKEN not set");
        return 1;
    }

    finam::auth::TokenManagerConfig cfg{
        .endpoint     = "api.finam.ru:443",
        .secret_token = secret,   // kept in memory, never logged
    };

    auto token_mgr = std::make_shared<finam::auth::TokenManager>(std::move(cfg));
    if (auto r = token_mgr->init(); !r) {
        spdlog::critical("Auth failed: {}", r.error().message);
        return 1;
    }

    spdlog::info("Authenticated. account_ids: {}",
                 token_mgr->account_ids().front());

    // TODO: init MarketDataClient, RiskManager, OrderExecutor, Strategy
    // TODO: main event loop (drain SPSCEventBus, call strategy)

    return 0;
}
