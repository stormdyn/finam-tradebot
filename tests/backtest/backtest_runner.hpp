#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <optional>
#include <cmath>
#include <spdlog/spdlog.h>
#include "core/interfaces.hpp"  // Signal, Bar

namespace finam::backtest {

// ── Типы ──────────────────────────────────────────────────────────────────────────

enum class TradeResult { TakeProfit, StopLoss, ForceClose };

struct BacktestResult {
    int    total_bars   {0};
    int    total_trades {0};
    int    wins         {0};
    int    losses       {0};
    double win_rate     {0.0};  // %
    double total_pnl_ticks {0.0};
    double max_win      {0.0};
    double max_loss     {0.0};
    double max_drawdown {0.0};
    double profit_factor{0.0};

    void print() const {
        spdlog::info("\n╔{:=<52}\u2557", "");
        spdlog::info("║ {:50} ║", "BACKTEST RESULTS");
        spdlog::info("╟{:-<52}\u2562", "");
        spdlog::info("║  Bars processed : {:>30}   ║", total_bars);
        spdlog::info("║  Total trades   : {:>30}   ║", total_trades);
        spdlog::info("║  Wins / Losses  : {:>14} / {:<13}  ║", wins, losses);
        spdlog::info("║  Win rate       : {:>29.1f}%  ║", win_rate);
        spdlog::info("║  Total PnL      : {:>27.0f} tk  ║", total_pnl_ticks);
        spdlog::info("║  Max win        : {:>27.0f} tk  ║", max_win);
        spdlog::info("║  Max loss       : {:>27.0f} tk  ║", max_loss);
        spdlog::info("║  Max drawdown   : {:>27.0f} tk  ║", max_drawdown);
        spdlog::info("║  Profit factor  : {:>30.2f}   ║", profit_factor);
        spdlog::info("╚{:=<52}\u255d", "");
    }
};

// ── Интерфейс стратегии для бэктеста ───────────────────────────────────────

struct IBacktestStrategy {
    virtual ~IBacktestStrategy() = default;
    // Получает D1-бар, возвращает Signal если есть сигнал, иначе nullopt
    [[nodiscard]] virtual std::optional<Signal> on_bar(const Bar&) = 0;
};

// ── BacktestRunner ──────────────────────────────────────────────────────────────────
//
// Запускает IBacktestStrategy на векторе Bar.
// Модель исполнения: одна позиция, вход по close следующего бара.
// SL/TP проверяются по high/low текущего бара.
//
// Трейдофф: внутрибаровый SL/TP не моделируется (D1 данные),
// поэтому equity curve оптимистичнее реальности.
// Для точного моделирования нужны минутные данные.

class BacktestRunner {
public:
    struct Config {
        double sl_ticks  {30.0};
        double tp_ticks  {90.0};
        double tick_size {1.0};
    };

    BacktestRunner(
        std::vector<Bar>      bars,
        IBacktestStrategy&    strategy,
        Config                cfg = {})
        : bars_(std::move(bars))
        , strategy_(strategy)
        , cfg_(cfg)
    {}

    [[nodiscard]] BacktestResult run();

private:
    void check_exit(const Bar&, BacktestResult&);
    void open_position(const Signal&, const Bar&, BacktestResult&);
    void close_position(double exit_price, std::string_view date,
                        TradeResult, BacktestResult&);
    void force_close(const Bar&, BacktestResult&);
    void finalize(BacktestResult&) const;

    std::vector<Bar>   bars_;
    IBacktestStrategy& strategy_;
    Config             cfg_;

    int         position_    {0};     // +1 long, -1 short, 0 flat
    double      entry_price_ {0.0};
    std::string entry_date_;
    double      equity_peak_ {0.0};
};

} // namespace finam::backtest
