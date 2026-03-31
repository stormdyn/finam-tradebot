#include <csignal>
#include <cstdlib>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
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

// ── Graceful shutdown ─────────────────────────────────────────────────────────
//
// FIX: POSIX гарантирует async-signal safety только для volatile sig_atomic_t.
// std::atomic<bool>::store() в signal handler — формально UB по C++ стандарту.
// Решение: sig_atomic_t как primary флаг (безопасен в handler), std::atomic<bool>
// как secondary для корректного happens-before в main loop.

static volatile sig_atomic_t g_shutdown_signal = 0;
static std::atomic<bool>     g_shutdown{false};

static void signal_handler(int) noexcept {
    g_shutdown_signal = 1;  // async-signal-safe
    g_shutdown.store(true, std::memory_order_relaxed);
}

// ── Logging ───────────────────────────────────────────────────────────────────
// Two sinks: stdout (color) + rotating file (5×10 MB).

static void setup_logging(bool debug_mode) {
    const auto level = debug_mode
        ? spdlog::level::debug
        : spdlog::level::info;

    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_level(level);
    console->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%t] %v");

    auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        "logs/tradebot.log", 10ULL * 1024 * 1024, 5);
    file->set_level(spdlog::level::debug);
    file->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");

    auto logger = std::make_shared<spdlog::logger>(
        "main", spdlog::sinks_init_list{console, file});
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    spdlog::flush_on(spdlog::level::warn);
}

// ── DryRunExecutor ────────────────────────────────────────────────────────────
// Logs orders without sending them to the exchange.

class DryRunExecutor final : public finam::IOrderExecutor {
public:
    // FIX: int64_t — приведено в соответствие с IOrderExecutor::submit и
    // Finam API (order_no — всегда 64-bit integer).
    finam::Result<int64_t> submit(const finam::OrderRequest& req) override {
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
    int64_t id_{0};  // FIX: int64_t вместо int32_t
};

// ── SymbolConfig ──────────────────────────────────────────────────────────────
// Per-symbol trading parameters. Add/remove entries in make_symbol_configs().

struct SymbolConfig {
    std::string  ticker;        // "Si", "RTS", "GOLD", "MIX"
    double       tick_size;     // tick size in roubles
    int32_t      base_qty;      // contracts per signal
    double       sl_ticks;      // stop-loss in ticks
    double       tp_ticks;      // take-profit in ticks
    int          rollover_days; // days before expiry to roll
};

// ── Active symbol list ────────────────────────────────────────────────────────
// Edit here to add or remove instruments. Symbols not listed are ignored.
//
// Tradeoff: static config vs runtime JSON — static is simpler, safer,
// and avoids parsing errors on startup. Recompilation takes <30s.

static std::vector<SymbolConfig> make_symbol_configs() {
    return {
        // ticker   tick   qty  sl    tp    roll
        { "Si",    1.0,    1,  30.0, 90.0,  5 },
        { "RTS",   10.0,   1,  30.0, 90.0,  5 },
        { "GOLD",  1.0,    1,  30.0, 90.0,  5 },
        // { "MIX",  0.05,  1,  30.0, 90.0,  5 },  // uncomment to enable
    };
}

int main(int argc, char* argv[]) {
    // ── CLI args ──────────────────────────────────────────────────────────────
    bool dry_run    = false;
    bool debug_mode = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--dry-run") { dry_run    = true; }
        if (arg == "--debug")   { debug_mode = true; }
    }

    setup_logging(debug_mode);
    spdlog::info("finam-tradebot starting{}{}",
        dry_run    ? " [DRY-RUN]" : "",
        debug_mode ? " [DEBUG]"   : "");

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Env ───────────────────────────────────────────────────────────────────
    const char* secret_env = std::getenv("FINAM_SECRET_TOKEN");
    if (!secret_env) {
        spdlog::critical("FINAM_SECRET_TOKEN not set");
        return 1;
    }

    // ── Auth ──────────────────────────────────────────────────────────────────
    auto token_mgr = std::make_shared<finam::auth::TokenManager>(
        finam::auth::TokenManagerConfig{.endpoint = "api.finam.ru:443", .use_tls = true}
    );
    if (auto r = token_mgr->init(secret_env); !r) {
        spdlog::critical("Auth failed: {}", r.error().message);
        return 1;
    }
    const std::string account_id{token_mgr->primary_account_id()};
    spdlog::info("Authenticated, account: {}", account_id);

    // ── Risk ──────────────────────────────────────────────────────────────────
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

    // ── Market data ───────────────────────────────────────────────────────────
    auto md = std::make_shared<finam::market_data::MarketDataClient>(token_mgr);

    // ── Executor ──────────────────────────────────────────────────────────────
    std::shared_ptr<finam::IOrderExecutor>     executor;
    std::shared_ptr<finam::order::OrderClient> order_client;
    if (dry_run) {
        executor = std::make_shared<DryRunExecutor>();
        spdlog::warn("[DryRun] orders will NOT be sent to exchange");
    } else {
        order_client = std::make_shared<finam::order::OrderClient>(token_mgr, account_id);
        executor     = order_client;
    }

    // ── Strategy runners (one per symbol) ────────────────────────────────────
    const auto symbol_cfgs = make_symbol_configs();
    std::vector<std::unique_ptr<finam::strategy::StrategyRunner>> runners;
    runners.reserve(symbol_cfgs.size());

    for (const auto& sc : symbol_cfgs) {
        const auto sym = finam::core::nearest_contract(sc.ticker, sc.rollover_days);
        spdlog::info("Adding instrument: {} (tick={} qty={} sl={} tp={})",
            sym.to_string(), sc.tick_size, sc.base_qty, sc.sl_ticks, sc.tp_ticks);

        runners.push_back(std::make_unique<finam::strategy::StrategyRunner>(
            finam::strategy::StrategyRunner::Config{
                .strategy = finam::strategy::ConfluenceStrategy::Config{
                    .symbol    = sym,
                    .base_qty  = sc.base_qty,
                    .sl_ticks  = sc.sl_ticks,
                    .tp_ticks  = sc.tp_ticks,
                    .tick_size = sc.tick_size,
                },
                .base_ticker   = sc.ticker,
                .history_days  = 20,
                .rollover_days = sc.rollover_days,
                .poll_interval = std::chrono::microseconds{100},
            },
            md, executor, token_mgr, risk
        ));
    }

    if (order_client) {
        std::vector<finam::order::OrderUpdateCallback> cbs;
        cbs.reserve(runners.size());
        for (auto& r : runners) {
            cbs.push_back(r->make_order_callback());
        }
        order_client->set_update_callback(
            [cbs = std::move(cbs)](const finam::OrderUpdate& upd) {
                for (const auto& cb : cbs) { cb(upd); }
            }
        );
    }

    // ── Health server ─────────────────────────────────────────────────────────
    finam::core::HealthServer health{8080, [&] {
        const auto st = risk->account_state();
        std::string symbols;
        for (const auto& sc : symbol_cfgs) {
            if (!symbols.empty()) { symbols += ','; }
            symbols += sc.ticker;
        }
        return finam::core::HealthStatus{
            .ok              = !risk->is_tripped(),
            .active_symbol   = symbols,
            .daily_pnl       = st.daily_pnl,
            .liquid_value    = st.liquid_value,
            .circuit_breaker = risk->is_tripped(),
        };
    }};
    health.start();

    spdlog::info("Bot running: {} instruments, health=http://localhost:8080/health",
        runners.size());

    // ── Main loop ─────────────────────────────────────────────────────────────
    // FIX: проверяем оба флага — sig_atomic_t (async-signal-safe) и atomic<bool>
    // (memory_order_acquire для корректного happens-before с другими потоками).
    while (!g_shutdown_signal && !g_shutdown.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::seconds{1});

    // ── Graceful shutdown ─────────────────────────────────────────────────────
    spdlog::info("Shutting down {} runners...", runners.size());
    health.stop();
    runners.clear();
    if (order_client) { order_client->shutdown(); }
    risk->shutdown();
    token_mgr->shutdown();
    spdlog::info("finam-tradebot stopped");
    return 0;
}
