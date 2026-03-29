#include <cstdlib>
#include <spdlog/spdlog.h>
#include "auth/token_manager.hpp"
#include "market_data/market_data_client.hpp"   // ← добавить

int main() {
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::info("finam-tradebot starting");

    const char* secret = std::getenv("FINAM_SECRET_TOKEN");
    if (!secret) {
        spdlog::critical("FINAM_SECRET_TOKEN env variable not set");
        return 1;
    }

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

    // ── MarketData ────────────────────────────────────────────────────────────
    auto md = std::make_shared<finam::market_data::MarketDataClient>(token_mgr);

    // Подписка на котировки Si
    auto quotes_sub = md->subscribe_quotes(
        { finam::Symbol{"Si-6.26", "FORTS"} },
        [](const finam::Quote& q) {
            spdlog::debug("[quote] {} bid={} ask={} last={}",
                q.symbol.to_string(), q.bid, q.ask, q.last);
        }
    );

    // Исторические свечи (stub — пока возвращает пустой вектор)
    auto now  = finam::Timestamp{};
    auto from = now - std::chrono::hours(24);
    if (auto bars = md->get_bars({"Si-6.26", "FORTS"}, "M5", from, now); bars)
        spdlog::info("get_bars: {} candles", bars->size());

    // ── TODO: OrderExecutor, RiskManager, Strategy ───────────────────────────

    std::this_thread::sleep_for(std::chrono::seconds(2));

    token_mgr->shutdown();
    spdlog::info("finam-tradebot stopped");
    return 0;
}
