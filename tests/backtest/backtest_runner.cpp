// backtest_runner.cpp
// Точка компиляции для backtest_runner.hpp.
// backtest_main.cpp содержит main(), здесь — реализация BacktestRunner.
// Разделение нужно чтобы избежать ODR-нарушений если runner.hpp
// подключать в других TU в будущем.

#include "backtest_runner.hpp"

namespace finam::backtest {

// ── BacktestRunner::run ───────────────────────────────────────────────────────────

BacktestResult BacktestRunner::run() {
    if (bars_.empty()) return {};

    BacktestResult result;
    result.total_bars = static_cast<int>(bars_.size());

    for (std::size_t i = 0; i < bars_.size(); ++i) {
        const auto& bar = bars_[i];

        // Обновляем состояние позиции (SL/TP check)
        if (position_ != 0) {
            check_exit(bar, result);
        }

        // Передаём бар стратегии
        if (auto sig = strategy_.on_bar(bar); sig && position_ == 0) {
            open_position(*sig, bar, result);
        }
    }

    // Принудительно закрываем остаток по цене закрытия
    if (position_ != 0) {
        force_close(bars_.back(), result);
    }

    finalize(result);
    return result;
}

// ── Проверка SL/TP ─────────────────────────────────────────────────────────────────

void BacktestRunner::check_exit(const Bar& bar, BacktestResult& result) {
    const double sl = (position_ > 0)
        ? entry_price_ - cfg_.sl_ticks * cfg_.tick_size
        : entry_price_ + cfg_.sl_ticks * cfg_.tick_size;
    const double tp = (position_ > 0)
        ? entry_price_ + cfg_.tp_ticks * cfg_.tick_size
        : entry_price_ - cfg_.tp_ticks * cfg_.tick_size;

    // В пределах одного бара проверяем SL сначала (консервативно: worst case)
    if (position_ > 0 && bar.low <= sl) {
        close_position(sl, bar.date, TradeResult::StopLoss, result);
    } else if (position_ > 0 && bar.high >= tp) {
        close_position(tp, bar.date, TradeResult::TakeProfit, result);
    } else if (position_ < 0 && bar.high >= sl) {
        close_position(sl, bar.date, TradeResult::StopLoss, result);
    } else if (position_ < 0 && bar.low <= tp) {
        close_position(tp, bar.date, TradeResult::TakeProfit, result);
    }
}

// ── Открытие/закрытие позиции ───────────────────────────────────────────────

void BacktestRunner::open_position(
    const Signal& sig, const Bar& bar, BacktestResult& result)
{
    // входим по open следующего бара (нельзя войти на сигнале того же бара)
    entry_price_ = bar.close;  // приближение — вход по close
    position_    = (sig.direction == Signal::Direction::Buy) ? 1 : -1;
    entry_date_  = bar.date;
    result.total_trades++;
    spdlog::debug("[BT] OPEN {} @ {:.0f}  ({})",
        position_ > 0 ? "LONG" : "SHORT", entry_price_, bar.date);
}

void BacktestRunner::close_position(
    double exit_price, std::string_view date,
    TradeResult tr, BacktestResult& result)
{
    const double pnl_ticks =
        (exit_price - entry_price_) / cfg_.tick_size * position_;
    result.total_pnl_ticks += pnl_ticks;

    if (pnl_ticks > 0)      result.wins++;
    else if (pnl_ticks < 0) result.losses++;

    result.max_win  = std::max(result.max_win,  pnl_ticks);
    result.max_loss = std::min(result.max_loss, pnl_ticks);

    // drawdown
    equity_peak_ = std::max(equity_peak_, result.total_pnl_ticks);
    const double dd = equity_peak_ - result.total_pnl_ticks;
    result.max_drawdown = std::max(result.max_drawdown, dd);

    spdlog::debug("[BT] CLOSE {} @ {:.0f}  pnl={:+.0f} ticks  ({})",
        tr == TradeResult::StopLoss ? "SL" : "TP",
        exit_price, pnl_ticks, date);

    position_    = 0;
    entry_price_ = 0.0;
}

void BacktestRunner::force_close(const Bar& bar, BacktestResult& result) {
    spdlog::debug("[BT] FORCE CLOSE @ {:.0f}", bar.close);
    close_position(bar.close, bar.date, TradeResult::ForceClose, result);
}

// ── Финализация ─────────────────────────────────────────────────────────────────────

void BacktestRunner::finalize(BacktestResult& result) const {
    const int total = result.wins + result.losses;
    result.win_rate = total > 0
        ? static_cast<double>(result.wins) / total * 100.0
        : 0.0;
    result.profit_factor = (result.max_loss < 0.0)
        ? result.max_win / std::abs(result.max_loss)
        : 0.0;
}

} // namespace finam::backtest
