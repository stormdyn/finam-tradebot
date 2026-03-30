#include <cstdlib>
#include <thread>
#include <spdlog/spdlog.h>
#include "auth/token_manager.hpp"
#include "market_data/market_data_client.hpp"
#include "order/order_client.hpp"

int main() {
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::info("finam-tradebot starting");

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

    const std::string account_id{token_mgr->primary_account_id()};

    // ── MarketData ────────────────────────────────────────────────────────────
    auto md = std::make_shared<finam::market_data::MarketDataClient>(token_mgr);

    auto quotes_sub = md->subscribe_quotes(
        { finam::Symbol{"Si-6.26", "FORTS"} },
        [](const finam::Quote& q) {
            spdlog::debug("[quote] {} bid={} ask={} last={}",
                q.symbol.to_string(), q.bid, q.ask, q.last);
        }
    );

    // ── Orders ────────────────────────────────────────────────────────────────
    auto order_client = std::make_shared<finam::order::OrderClient>(
        token_mgr,
        account_id,
        [](const finam::OrderUpdate& upd) {
            spdlog::info("[order_update] local_id={} symbol={} status={}",
                upd.transaction_id,
                upd.symbol.to_string(),
                static_cast<int>(upd.status));
        }
    );

    // Тест: submit лимитной заявки
    auto result = order_client->submit(finam::OrderRequest{
        .client_id = account_id,
        .symbol    = {"Si-6.26", "FORTS"},
        .side      = finam::OrderSide::Buy,
        .type      = finam::OrderType::Limit,
        .price     = 85000.0,
        .quantity  = 1,
    });

    if (result)
        spdlog::info("Order submitted, local_id={}", *result);
    else
        spdlog::error("Order failed: {}", result.error().message);

    // Проверяем трекер
    auto active = order_client->active_orders();
    spdlog::info("Active orders: {}", active.size());

    // Тест: отмена
    order_client->cancel(0, account_id);
    spdlog::info("Active after cancel: {}", order_client->active_orders().size());

    std::this_thread::sleep_for(std::chrono::seconds{1});

    token_mgr->shutdown();
    spdlog::info("finam-tradebot stopped");
    return 0;
}