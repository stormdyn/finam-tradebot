#include <csignal>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <spdlog/spdlog.h>

#include "auth/token_manager.hpp"
#include "market_data/market_data_client.hpp"
#include "order/order_client.hpp"
#include "risk/risk_manager.hpp"
#include "strategy/strategy_runner.hpp"

// ── Graceful shutdown ─────────────────────────────────────────────────────────────────

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

    // ── Env ──────────────────────────────────────────────────────────────────────────
    const char* secret_env = std::getenv("FINAM_SECRET_TOKEN");
    if (!secret_env) {
        spdlog::critical("FINAM_SECRET_TOKEN not set");
        return 1;
    }

    // ── Auth ───────────────────────────────────────────────────────────────────────
    auto token_mgr = std::make_shared<finam::auth::TokenManager>(
        finam::auth::TokenManagerConfig{
            .endpoint = "api.finam.ru:443",
            .use_tls  = true,
        }
    );
    if (auto r = token_mgr->init(secret_env); !r) {
        spdlog::critical("Auth failed: {}", r.error().message);
        return 1;
    }
    const std::string account_id{token_mgr->primary_account_id()};
    spdlog::info("Authenticated, account: {}", account_id);

    // ── Risk ───────────────────────────────────────────────────────────────────────
    auto risk = std::make_shared<finam::risk::RiskManager>(
        finam::risk::RiskConfig{
            .per_trade_pct      = 2.0,
            .max_daily_loss_pct = 5.0,
            .max_drawdown_pct   = 15.0,
            .max_positions      = 3,
            .require_stop_loss  = true,
            .rollover_days      = 5,
        },
        token_mgr
    );
    risk->start();

    // ── Market data ─────────────────────────────────────────────────────────────
    auto md = std::make_shared<finam::market_data::MarketDataClient>(token_mgr);

    // ── OrderClient ───────────────────────────────────────────────────────────────────
    //
    // Конструируем OrderClient без callback — подключим позже через StrategyRunner.
    // Порядок важен: runner владеет стратегией и знает куда маршрутизировать on_order_update.
    auto order_client = std::make_shared<finam::order::OrderClient>(
        token_mgr, account_id
        // callback передаётся позже через set_update_callback()
    );

    // ── StrategyRunner ─────────────────────────────────────────────────────────────
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

    // Runner создаётся до set_update_callback — потому принимает order_client по IOrderExecutor.
    // StrategyRunner сам регистрирует on_update_ через set_update_callback() в конструкторе.
    auto runner = std::make_unique<finam::strategy::StrategyRunner>(
        runner_cfg, md, order_client, token_mgr, risk
    );

    // Теперь вызываем set_update_callback: runner уже готов, strategy_thread_ запущен.
    // Коллбэк вызывается из order stream thread OrderClient —
    // он доставляет событие в strategy thread через SPSC-очередь.
    order_client->set_update_callback(runner->make_order_callback());

    spdlog::info("Strategy running, press Ctrl+C to stop");

    // ── Main loop ───────────────────────────────────────────────────────────────────
    while (!g_shutdown.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::seconds{1});

    // ── Graceful shutdown ─────────────────────────────────────────────────────────────
    spdlog::info("Shutting down...");
    runner.reset();          // join strategy_thread_, cancel subscriptions
    order_client->shutdown(); // join order stream
    risk->shutdown();         // join poll_thread_
    token_mgr->shutdown();    // join renewal_thread_
    spdlog::info("finam-tradebot stopped");
    return 0;
}
