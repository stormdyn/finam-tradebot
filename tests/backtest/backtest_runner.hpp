#pragma once
#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <spdlog/spdlog.h>

#include "core/interfaces.hpp"   // Bar, Signal
#include "strategy/confluence_strategy.hpp"

namespace finam::backtest {

// ── Типы ─────────────────────────────────────────────────────────────────────────

enum class ExitReason { TakeProfit, StopLoss, ForceClose };

struct Trade {
    std::string entry_date;
    std::string exit_date;
    double      entry_price {0.0};
    double      exit_price  {0.0};
    int         direction   {0};   // +1 long, -1 short
    double      pnl_ticks   {0.0};
    ExitReason  reason      {ExitReason::ForceClose};
};

struct BacktestResult {
    int    total_bars      {0};
    int    total_trades    {0};
    int    wins            {0};
    int    losses          {0};
    double win_rate        {0.0};  // %
    double total_pnl_ticks {0.0};
    double max_win         {0.0};
    double max_loss        {0.0};
    double max_drawdown    {0.0};
    double profit_factor   {0.0};
    std::vector<Trade> trades;

    void print() const {
        spdlog::info("");
        spdlog::info("╔{:=<54}\u2557", "");
        spdlog::info("║  {:<52}  ║", "BACKTEST RESULTS");
        spdlog::info("╟{:-<54}\u2562", "");
        spdlog::info("║  Bars processed : {:<33}  ║", total_bars);
        spdlog::info("║  Total trades   : {:<33}  ║", total_trades);
        spdlog::info("║  Wins / Losses  : {} / {:<27}  ║", wins, losses);
        spdlog::info("║  Win rate       : {:<32.1f}%  ║", win_rate);
        spdlog::info("║  Total PnL      : {:<30.0f} tk  ║", total_pnl_ticks);
        spdlog::info("║  Max win        : {:<30.0f} tk  ║", max_win);
        spdlog::info("║  Max loss       : {:<30.0f} tk  ║", max_loss);
        spdlog::info("║  Max drawdown   : {:<30.0f} tk  ║", max_drawdown);
        spdlog::info("║  Profit factor  : {:<33.2f}  ║", profit_factor);
        spdlog::info("╚{:=<54}\u255d", "");
    }

    void to_csv(const std::string& path) const {
        std::ofstream f{path};
        if (!f) { spdlog::error("[BT] cannot write {}", path); return; }
        f << "entry_date,exit_date,direction,entry_price,exit_price,"
             "pnl_ticks,reason\n";
        for (const auto& t : trades) {
            f << t.entry_date << ',' << t.exit_date << ','
              << (t.direction > 0 ? "LONG" : "SHORT") << ','
              << t.entry_price << ',' << t.exit_price << ','
              << t.pnl_ticks << ','
              << (t.reason == ExitReason::TakeProfit ? "TP" :
                  t.reason == ExitReason::StopLoss   ? "SL" : "FC")
              << '\n';
        }
        spdlog::info("[BT] trades saved → {}", path);
    }
};

// ── load_csv ──────────────────────────────────────────────────────────────────────────
//
// Читает CSV: date,open,high,low,close,volume
// Попускает заголовок и пустые строки.

[[nodiscard]] inline std::vector<Bar> load_csv(const std::string& path) {
    std::ifstream f{path};
    if (!f) {
        spdlog::error("[BT] cannot open {}", path);
        return {};
    }
    std::vector<Bar> bars;
    std::string line;
    std::getline(f, line);  // заголовок
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::istringstream ss{line};
        std::string date, o, h, l, c, v;
        if (!std::getline(ss, date, ',') ||
            !std::getline(ss, o, ',')    ||
            !std::getline(ss, h, ',')    ||
            !std::getline(ss, l, ',')    ||
            !std::getline(ss, c, ',')    ||
            !std::getline(ss, v, ',')) continue;
        try {
            bars.push_back(Bar{
                .date   = date,
                .open   = std::stod(o),
                .high   = std::stod(h),
                .low    = std::stod(l),
                .close  = std::stod(c),
                .volume = static_cast<int64_t>(std::stoll(v)),
            });
        } catch (...) { continue; }
    }
    spdlog::info("[BT] loaded {} bars from {}", bars.size(), path);
    return bars;
}

// ── BacktestRunner ──────────────────────────────────────────────────────────────────
//
// runner{strategy, config}  →  result = runner.run(bars)
//
// Модель: вход по bar.close, SL/TP по high/low текущего бара (худший случай).
// Трейдофф: D1 — оптимистичнее реальности (внутрибаровый путь не моделируется).
// Комиссия вычитается в тиках с каждой сделки.

class BacktestRunner {
public:
    struct Config {
        double tick_size  {1.0};
        double slippage   {0.0};  // тиков на вход/выход
        double commission {0.0};  // тиков на сделку (round-trip / 2)
    };

    BacktestRunner(
        strategy::ConfluenceStrategy& strategy,
        Config                        cfg = {})
        : strategy_(strategy)
        , cfg_(cfg)
    {}

    [[nodiscard]] BacktestResult run(const std::vector<Bar>& bars);

private:
    void check_exit(const Bar&, BacktestResult&);
    void open_position(const Signal&, const Bar&, BacktestResult&);
    void close_position(double price, const Bar&, ExitReason, BacktestResult&);
    void force_close(const Bar&, BacktestResult&);
    void finalize(BacktestResult&) const;

    strategy::ConfluenceStrategy& strategy_;
    Config      cfg_;
    int         position_    {0};
    double      entry_price_ {0.0};
    std::string entry_date_;
    double      equity_peak_ {0.0};
};

} // namespace finam::backtest
