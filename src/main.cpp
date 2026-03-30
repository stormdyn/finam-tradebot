#include <csignal>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <spdlog/spdlog.h>

#include "auth/token_manager.hpp"
#include "market_data/market_data_client.hpp"
#include "order/order_client.hpp"
#include "strategy/strategy_runner.hpp"

// ── Graceful shutdown ────────────────────────────────────────────────────────────
static std::atomic<bool> g_shutdown{false};

static void signal_handler(int) noexcept {
    g_shutdown.store(true, std::memory_order_release);
}

int main() {
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] [%t] %v");
    spdlog::info("finam-tradebot starting");

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Env ───────────────────────────────────────────────────────────────────────────
    const char* secret_env = std::getenv("FINAM_SECRET_TOKEN");
    if (!secret_env) {
        spdlog::critical("FINAM_SECRET_TOKEN not set");
        return 1;
    }
    const std::string_view secret{secret_env};

    // ── Auth ────────────────────────────────────────────────────────────────────────
    auto token_mgr = std::make_shared<finam::auth::TokenManager>(
        finam::auth::TokenManagerConfig{
            .endpoint = "api.finam.ru:443",
            .use_tls  = true,
        }
    );
    // secret передаётся в init(), не хранится в Config
    if (auto r = token_mgr->init(secret); !r) {
        spdlog::critical("Auth failed: {}", r.error().message);
        return 1;
    }
    spdlog::info("Authenticated, account: {}", token_mgr->primary_account_id());

    // ── Market data ─────────────────────────────────────────────────────────────
    auto md = std::make_shared<finam::market_data::MarketDataClient>(token_mgr);

    // ── Order executor ────────────────────────────────────────────────────────────
    const std::string account_id{token_mgr->primary_account_id()};

    auto order_client = std::make_shared<finam::order::OrderClient>(
        token_mgr,
        account_id,
        [](const finam::OrderUpdate& upd) {
            spdlog::info("[order] id={} symbol={} status={} filled={}",
                upd.transaction_id,
                upd.symbol.to_string(),
                static_cast<int>(upd.status),
                upd.qty_filled);
        }
    );

    // ── Strategy ───────────────────────────────────────────────────────────────────
    finam::strategy::StrategyRunner::Config runner_cfg{
        .strategy = finam::strategy::ConfluenceStrategy::Config{
            .symbol    = finam::Symbol{"Si-6.26", "FORTS"},
            .base_qty  = 1,
            .sl_ticks  = 30.0,
            .tp_ticks  = 90.0,
            .tick_size = 1.0,
        },
        .history_days  = 20,
        .poll_interval = std::chrono::microseconds{100},
    };

    auto runner = std::make_unique<finam::strategy::StrategyRunner>(
        runner_cfg, md, order_client, token_mgr
    );

    spdlog::info("Strategy running, press Ctrl+C to stop");

    // ── Main loop ───────────────────────────────────────────────────────────────────
    while (!g_shutdown.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::seconds{1});

    // ── Graceful shutdown ───────────────────────────────────────────────────────────
    spdlog::info("Shutting down...");
    runner.reset();
    token_mgr->shutdown();
    spdlog::info("finam-tradebot stopped");
    return 0;
}
