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

static volatile sig_atomic_t g_shutdown_signal = 0;
static std::atomic<bool>     g_shutdown{false};

static void signal_handler(int) noexcept {
    g_shutdown_signal = 1;
    g_shutdown.store(true, std::memory_order_relaxed);
}

static void setup_logging(bool debug_mode) {
    const auto level = debug_mode ? spdlog::level::debug : spdlog::level::info;
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

class DryRunExecutor final : public finam::IOrderExecutor {
public:
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
    int64_t id_{0};
};

// ── Настройка инструментов ─────────────────────────────────────────────────────
//
// ticker     — ROOT-часть тикера (перед буквой квартала)
// tick_size  — шаг цены в единицах котировки (lot_size/decimals из GetAsset)
// base_qty   — контрактов на сигнал
// sl_ticks   — стоп-лосс в тиках
// tp_ticks   — тейк-профит в тиках
// rollover   — дней до экспирации для ролловера
//
// Тикеры и параметры подтверждены через GetAsset (expiry, decimals, lot).
// MIC = RTSX («МОСКОВСКАЯ БИРЖА — СРОЧНЫЙ РЫНОК»)
//
//  ──────────────────────────────────────────────────────────────────────────
//  ticker  tick    qty  sl     tp    roll   комментарий (board=FUT)
struct SymbolConfig {
    std::string  ticker;
    double       tick_size;
    int32_t      base_qty;
    double       sl_ticks;
    double       tp_ticks;
    int          rollover_days;
};

static std::vector<SymbolConfig> make_symbol_configs() {
    return {
        //── ВАЛЮТНЫЕ ────────────────────────────────────────────────────────
        { "Si",   1.0,   1,  30.0,  90.0,  5 }, // USD/RUB   SiM6@RTSX
        { "Eu",   1.0,   1,  30.0,  90.0,  5 }, // EUR/RUB   EuM6@RTSX
        { "Cn",   0.001, 1,  30.0,  90.0,  5 }, // CNY/RUB   CnM6@RTSX
        //── ИНДЕКСЫ ───────────────────────────────────────────────────────
        { "RI",   10.0,  1,  30.0,  90.0,  5 }, // Индекс РТС   RIM6@RTSX
        { "MX",   0.05,  1,  30.0,  90.0,  5 }, // Индекс МОСБИРЖИ MXM6@RTSX
        //── ТОВАРЫ ─────────────────────────────────────────────────────────
        { "GD",   1.0,   1,  30.0,  90.0,  5 }, // Золото     GDM6@RTSX
        { "BR",   0.01,  1,  30.0,  90.0,  5 }, // Нефть Brent BRM6@RTSX
        { "Ng",   0.001, 1,  30.0,  90.0,  5 }, // Газ природный NgM6@RTSX
        { "SV",   1.0,   1,  30.0,  90.0,  5 }, // Серебро    SVM6@RTSX
        { "PL",   0.1,   1,  30.0,  90.0,  5 }, // Платина    PLM6@RTSX
        { "Cu",   10.0,  1,  30.0,  90.0,  5 }, // Медь      CuM6@RTSX
        { "Al",   5.0,   1,  30.0,  90.0,  5 }, // Алюминий  AlM6@RTSX
        //── АКЦИИ ─────────────────────────────────────────────────────────
        { "GZ",   1.0,   1,  30.0,  90.0,  5 }, // Газпром    GZM6@RTSX
        { "LK",   1.0,   1,  30.0,  90.0,  5 }, // ЛУКОЙЛ    LKM6@RTSX
        { "SR",   0.5,   1,  30.0,  90.0,  5 }, // Сбербанк   SRM6@RTSX
        { "VB",   0.001, 1,  30.0,  90.0,  5 }, // ВТБ      VBM6@RTSX
        { "RN",   1.0,   1,  30.0,  90.0,  5 }, // Роснефть  RNM6@RTSX
        { "GM",   1.0,   1,  30.0,  90.0,  5 }, // ГМК-НОР    GMM6@RTSX (ГМК)
        { "Ym",   0.1,   1,  30.0,  90.0,  5 }, // Золото/Рубь YmM6@RTSX (GLDRUB)
    };
}

int main(int argc, char* argv[]) {
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

    const char* secret_env = std::getenv("FINAM_SECRET_TOKEN");
    if (!secret_env) {
        spdlog::critical("FINAM_SECRET_TOKEN not set");
        return 1;
    }

    auto token_mgr = std::make_shared<finam::auth::TokenManager>(
        finam::auth::TokenManagerConfig{.endpoint = "api.finam.ru:443", .use_tls = true}
    );
    if (auto r = token_mgr->init(secret_env); !r) {
        spdlog::critical("Auth failed: {}", r.error().message);
        return 1;
    }
    const std::string account_id{token_mgr->primary_account_id()};
    spdlog::info("Authenticated, account: {}", account_id);

    auto risk = std::make_shared<finam::risk::RiskManager>(
        finam::risk::RiskConfig{
            .per_trade_pct      = 2.0,
            .max_daily_loss_pct = 5.0,
            .max_drawdown_pct   = 15.0,
            .max_positions      = 5,
            .require_stop_loss  = true,
            .rollover_days      = 5,
        },
        token_mgr
    );
    risk->start();

    auto md = std::make_shared<finam::market_data::MarketDataClient>(token_mgr);

    std::shared_ptr<finam::IOrderExecutor>     executor;
    std::shared_ptr<finam::order::OrderClient> order_client;
    if (dry_run) {
        executor = std::make_shared<DryRunExecutor>();
        spdlog::warn("[DryRun] orders will NOT be sent to exchange");
    } else {
        order_client = std::make_shared<finam::order::OrderClient>(token_mgr, account_id);
        executor     = order_client;
    }

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
        for (auto& r : runners) cbs.push_back(r->make_order_callback());
        order_client->set_update_callback(
            [cbs = std::move(cbs)](const finam::OrderUpdate& upd) {
                for (const auto& cb : cbs) cb(upd);
            }
        );
    }

    finam::core::HealthServer health{8080, [&] {
        const auto st = risk->account_state();
        std::string symbols;
        for (const auto& sc : symbol_cfgs) {
            if (!symbols.empty()) symbols += ',';
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

    while (!g_shutdown_signal && !g_shutdown.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::seconds{1});

    spdlog::info("Shutting down {} runners...", runners.size());
    health.stop();
    runners.clear();
    if (order_client) order_client->shutdown();
    risk->shutdown();
    token_mgr->shutdown();
    spdlog::info("finam-tradebot stopped");
    return 0;
}
