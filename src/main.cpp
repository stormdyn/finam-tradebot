#include <csignal>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <spdlog/spdlog.h>

#include "auth/token_manager.hpp"
#include "core/contract.hpp"
#include "market_data/market_data_client.hpp"
#include "order/order_client.hpp"
#include "risk/risk_manager.hpp"
#include "strategy/strategy_runner.hpp"

// ── Graceful shutdown ─────────────────────────────────────────────────────────────────

static std::atomic<bool> g_shutdown{false};

static void signal_handler(int) noexcept {
    g_shutdown.store(true, std::memory_order_release);
}

// ── dry-run guard ────────────────────────────────────────────────────────────────────
//
// Обёртка IOrderExecutor, которая логирует ордера, но не отправляет их.
// Используется в режиме --dry-run вместо OrderClient.

class DryRunExecutor final : public finam::IOrderExecutor {
public:
    finam::Result<int32_t> submit(const finam::OrderRequest& req) override {
        spdlog::info("[DryRun] SUBMIT symbol={} side={} type={} qty={} price={}",
            req.symbol.to_string(),
            req.side == finam::OrderSide::Buy ? "BUY" : "SELL",
            req.type == finam::OrderType::Market ? "MARKET" : "LIMIT",
            req.quantity, req.price);
        return ++id_;
    }

    finam::Result<void> cancel(int64_t order_no, std::string_view) override {
        spdlog::info("[DryRun] CANCEL order_no={}", order_no);
        return {};
    }

private:
    int32_t id_{0};
};

int main(int argc, char* argv[]) {
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] [%t] %v");

    // ── Разбор аргументов ────────────────────────────────────────────────────────────
    bool dry_run = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--dry-run") dry_run = true;
    }

    spdlog::info("finam-tradebot starting{}", dry_run ? " [DRY-RUN]" : "");
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

    // ── Автовыбор символа ───────────────────────────────────────────────────────────
    // nearest_contract("Si", rollover_days=5):
    //   30 марта 2026, third_friday(2026,3)=20, cur_day(30)>=20-5=15 → Si-6.26@FORTS
    const auto symbol = finam::core::nearest_contract("Si", 5);
    spdlog::info("Active contract: {}", symbol.to_string());

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

    // ── Executor: dry-run или реальный ───────────────────────────────────────────────
    std::shared_ptr<finam::IOrderExecutor> executor;
    std::shared_ptr<finam::order::OrderClient> order_client;

    if (dry_run) {
        executor = std::make_shared<DryRunExecutor>();
        spdlog::warn("[DryRun] orders will NOT be sent to exchange");
    } else {
        order_client = std::make_shared<finam::order::OrderClient>(
            token_mgr, account_id);
        executor = order_client;
    }

    // ── StrategyRunner ─────────────────────────────────────────────────────────────
    finam::strategy::StrategyRunner::Config runner_cfg{
        .strategy = finam::strategy::ConfluenceStrategy::Config{
            .symbol    = symbol,  // автовыбранный контракт
            .base_qty  = 1,
            .sl_ticks  = 30.0,
            .tp_ticks  = 90.0,
            .tick_size = 1.0,
        },
        .history_days  = 20,
        .poll_interval = std::chrono::microseconds{100},
    };

    auto runner = std::make_unique<finam::strategy::StrategyRunner>(
        runner_cfg, md, executor, token_mgr, risk
    );

    // Завязываем callback только если есть реальный OrderClient
    if (order_client)
        order_client->set_update_callback(runner->make_order_callback());

    spdlog::info("Strategy running on {}, press Ctrl+C to stop",
        symbol.to_string());

    // ── Main loop ───────────────────────────────────────────────────────────────────
    while (!g_shutdown.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::seconds{1});

    // ── Graceful shutdown ─────────────────────────────────────────────────────────────
    spdlog::info("Shutting down...");
    runner.reset();
    if (order_client) order_client->shutdown();
    risk->shutdown();
    token_mgr->shutdown();
    spdlog::info("finam-tradebot stopped");
    return 0;
}
