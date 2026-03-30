#include "backtest_runner.hpp"

namespace finam::backtest {

BacktestResult BacktestRunner::run(const std::vector<Bar>& bars) {
    BacktestResult result;
    result.total_bars = static_cast<int>(bars.size());

    for (const auto& bar : bars) {
        strategy_.on_bar(bar);  // обновляем индикаторы

        if (position_ != 0)
            check_exit(bar, result);

        // Открываем позицию только если флэт
        if (position_ == 0) {
            const auto sig = strategy_.on_quote(Quote{
                .bid = bar.close - cfg_.tick_size,
                .ask = bar.close + cfg_.tick_size,
            });
            if (sig.direction != Signal::Direction::None
                && sig.direction != Signal::Direction::Close)
            {
                open_position(sig, bar, result);
            }
        }
    }

    if (position_ != 0)
        force_close(bars.back(), result);

    finalize(result);
    return result;
}

void BacktestRunner::check_exit(const Bar& bar, BacktestResult& result) {
    const double sl = (position_ > 0)
        ? entry_price_ - cfg_.sl_ticks * cfg_.tick_size   // заполняем cfg_.sl_ticks
        : entry_price_ + cfg_.sl_ticks * cfg_.tick_size;
    const double tp = (position_ > 0)
        ? entry_price_ + cfg_.tp_ticks * cfg_.tick_size
        : entry_price_ - cfg_.tp_ticks * cfg_.tick_size;

    // SL проверяем сначала (worst case)
    if (position_ > 0 && bar.low  <= sl)
        close_position(sl, bar, ExitReason::StopLoss,   result);
    else if (position_ > 0 && bar.high >= tp)
        close_position(tp, bar, ExitReason::TakeProfit, result);
    else if (position_ < 0 && bar.high >= sl)
        close_position(sl, bar, ExitReason::StopLoss,   result);
    else if (position_ < 0 && bar.low  <= tp)
        close_position(tp, bar, ExitReason::TakeProfit, result);
}

void BacktestRunner::open_position(
    const Signal& sig, const Bar& bar, BacktestResult& result)
{
    const double slip = cfg_.slippage * cfg_.tick_size;
    entry_price_ = bar.close + (sig.direction == Signal::Direction::Buy ? slip : -slip);
    position_    = (sig.direction == Signal::Direction::Buy) ? 1 : -1;
    entry_date_  = bar.date;
    result.total_trades++;
    spdlog::debug("[BT] OPEN {} @ {:.0f}  ({})",
        position_ > 0 ? "LONG" : "SHORT", entry_price_, bar.date);
}

void BacktestRunner::close_position(
    double price, const Bar& bar, ExitReason reason, BacktestResult& result)
{
    const double slip     = cfg_.slippage * cfg_.tick_size;
    const double exit_px  = price + (position_ > 0 ? -slip : slip);
    const double comm     = cfg_.commission * 2.0;  // round-trip
    const double pnl      =
        (exit_px - entry_price_) / cfg_.tick_size * position_ - comm;

    result.total_pnl_ticks += pnl;
    if (pnl > 0) result.wins++;
    else         result.losses++;
    result.max_win  = std::max(result.max_win,  pnl);
    result.max_loss = std::min(result.max_loss, pnl);

    equity_peak_ = std::max(equity_peak_, result.total_pnl_ticks);
    result.max_drawdown = std::max(result.max_drawdown,
        equity_peak_ - result.total_pnl_ticks);

    result.trades.push_back(Trade{
        .entry_date  = entry_date_,
        .exit_date   = bar.date,
        .entry_price = entry_price_,
        .exit_price  = exit_px,
        .direction   = position_,
        .pnl_ticks   = pnl,
        .reason      = reason,
    });

    spdlog::debug("[BT] CLOSE {} @ {:.0f}  pnl={:+.1f} tk  ({})",
        reason == ExitReason::StopLoss ? "SL" :
        reason == ExitReason::TakeProfit ? "TP" : "FC",
        exit_px, pnl, bar.date);

    position_    = 0;
    entry_price_ = 0.0;
}

void BacktestRunner::force_close(const Bar& bar, BacktestResult& result) {
    close_position(bar.close, bar, ExitReason::ForceClose, result);
}

void BacktestRunner::finalize(BacktestResult& result) const {
    const int total    = result.wins + result.losses;
    result.win_rate    = total > 0
        ? static_cast<double>(result.wins) / total * 100.0 : 0.0;
    const double gross_loss = result.losses > 0
        ? std::abs(result.max_loss) * result.losses : 1.0;
    result.profit_factor = (result.wins > 0 && gross_loss > 0)
        ? (result.max_win * result.wins) / gross_loss : 0.0;
}

} // namespace finam::backtest
