#include <csignal>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "auth/token_manager.hpp"
#include "core/contract.hpp"
#include "core/health_server.hpp"
#include "market_data/market_data_client.hpp"
#include "order/order_client.hpp"
#include "risk/risk_manager.hpp"
#include "strategy/strategy_runner.hpp"

// ── Graceful shutdown ─────────────────────────────────────────────────────────────

static std::atomic<bool> g_shutdown{false};
static void signal_handler(int) noexcept {
    g_shutdown.store(true, std::memory_order_release);
}

// ── Настройка логгера ──────────────────────────────────────────────────────────────
//
// Два sink-а: stdout (color) + rotating file (5 файлов по 10 MB).
// Файлы пишутся в logs/ — монтируем через Docker volume.
// В release-сборке уровень info, в debug-сборке — debug.

static void setup_logging(bool debug_mode) {
    const auto level = debug_mode
        ? spdlog::level::debug
        : spdlog::level::info;

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(level);
    console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%t] %v");

    // 5 файлов по 10 MB = 50 MB макс. Дата в имени файла через pattern
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        "logs/tradebot.log",
        10ULL * 1024 * 1024,  // 10 MB
        5                      // 5 файлов
    );
    file_sink->set_level(spdlog::level::debug);  // в файл — всегда debug
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");

    auto logger = std::make_shared<spdlog::logger>(
        "main",
        spdlog::sinks_init_list{console_sink, file_sink}
    );
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    spdlog::flush_on(spdlog::level::warn);  // flush сразу на warn+
}

// ── DryRunExecutor ─────────────────────────────────────────────────────────────────────

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
    // ── Аргументы ───────────────────────────────────────────────────────────────────
    bool dry_run    = false;
    bool debug_mode = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--dry-run") dry_run    = true;
        if (arg == "--debug")   debug_mode = true;
    }

    setup_logging(debug_mode);
    spdlog::info("finam-tradebot starting{}{}",
        dry_run    ? " [DRY-RUN]" : "",
        debug_mode ? " [DEBUG]"   : "");

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
        finam::auth::TokenManagerConfig{.endpoint = "api.finam.ru:443", .use_tls = true}
    );
    if (auto r = token_mgr->init(secret_env); !r) {
        spdlog::critical("Auth failed: {}", r.error().message);
        return 1;
    }
    const std::string account_id{token_mgr->primary_account_id()};
    spdlog::info("Authenticated, account: {}", account_id);

    // ── Автовыбор символа ───────────────────────────────────────────────────────────
    const auto symbol = finam::core::nearest_contract("Si", 5);
    spdlog::info("Active contract: {}", symbol.to_string());

    // ── Risk ───────────────────────────────────────────────────────────────────────
    auto risk = std::make_shared<finam::risk::RiskManager>(
        finam::risk::RiskConfig{
            .per_trade_pct = 2.0, .max_daily_loss_pct = 5.0,
            .max_drawdown_pct = 15.0, .max_positions = 3,
            .require_stop_loss = true, .rollover_days = 5,
        },
        token_mgr
    );
    risk->start();

    // ── Market data ─────────────────────────────────────────────────────────────
    auto md = std::make_shared<finam::market_data::MarketDataClient>(token_mgr);

    // ── Executor ─────────────────────────────────────────────────────────────────────
    std::shared_ptr<finam::IOrderExecutor>     executor;
    std::shared_ptr<finam::order::OrderClient> order_client;
    if (dry_run) {
        executor = std::make_shared<DryRunExecutor>();
        spdlog::warn("[DryRun] orders will NOT be sent to exchange");
    } else {
        order_client = std::make_shared<finam::order::OrderClient>(token_mgr, account_id);
        executor     = order_client;
    }

    // ── StrategyRunner ────────────────────────────────────────────────────────────
    auto runner = std::make_unique<finam::strategy::StrategyRunner>(
        finam::strategy::StrategyRunner::Config{
            .strategy = finam::strategy::ConfluenceStrategy::Config{
                .symbol = symbol, .base_qty = 1,
                .sl_ticks = 30.0, .tp_ticks = 90.0, .tick_size = 1.0,
            },
            .history_days = 20, .rollover_days = 5,
            .poll_interval = std::chrono::microseconds{100},
        },
        md, executor, token_mgr, risk
    );
    if (order_client)
        order_client->set_update_callback(runner->make_order_callback());

    // ── Health server ─────────────────────────────────────────────────────────────
    finam::core::HealthServer health{8080, [&] {
        const auto st = risk->account_state();
        return finam::core::HealthStatus{
            .ok              = !risk->is_tripped(),
            .active_symbol   = symbol.to_string(),
            .daily_pnl       = st.daily_pnl,
            .liquid_value    = st.liquid_value,
            .circuit_breaker = risk->is_tripped(),
        };
    }};
    health.start();

    spdlog::info("Bot running: contract={} health=http://localhost:8080/health",
        symbol.to_string());

    // ── Main loop ─────────────────────────────────────────────────────────────────
    while (!g_shutdown.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::seconds{1});

    // ── Graceful shutdown ───────────────────────────────────────────────────────────
    spdlog::info("Shutting down...");
    health.stop();
    runner.reset();
    if (order_client) order_client->shutdown();
    risk->shutdown();
    token_mgr->shutdown();
    spdlog::info("finam-tradebot stopped");
    return 0;
}
