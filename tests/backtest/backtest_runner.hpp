#pragma once
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <functional>
#include <spdlog/spdlog.h>

#include "core/interfaces.hpp"
#include "strategy/confluence_strategy.hpp"

namespace finam::backtest {

// ── BacktestResult ───────────────────────────────────────────────────────────────

struct TradeRecord {
    std::chrono::system_clock::time_point entry_ts;
    std::chrono::system_clock::time_point exit_ts;
    double   entry_price{0.0};
    double   exit_price{0.0};
    int32_t  qty{0};
    int32_t  side{0};     // +1 long, -1 short
    double   pnl{0.0};   // в тиках
    std::string exit_reason;
};

struct BacktestResult {
    int32_t  total_trades{0};
    int32_t  win_trades{0};
    int32_t  loss_trades{0};
    double   win_rate{0.0};     // [0, 1]
    double   total_pnl{0.0};   // тики
    double   avg_win{0.0};
    double   avg_loss{0.0};
    double   profit_factor{0.0};
    double   max_drawdown{0.0}; // тики
    std::vector<TradeRecord> trades;

    void print() const {
        spdlog::info("\n=== Backtest Results ===");
        spdlog::info("  Trades:       {}", total_trades);
        spdlog::info("  Win rate:     {:.1f}%", win_rate * 100.0);
        spdlog::info("  Total PnL:    {:.1f} ticks", total_pnl);
        spdlog::info("  Avg Win:      {:.1f} | Avg Loss: {:.1f}", avg_win, avg_loss);
        spdlog::info("  Profit Factor:{:.2f}", profit_factor);
        spdlog::info("  Max Drawdown: {:.1f} ticks", max_drawdown);
    }

    // Вывод в CSV для дальнейшего анализа (Jupyter, Excel)
    void to_csv(const std::string& path) const {
        std::ofstream f(path);
        f << "entry_price,exit_price,qty,side,pnl,exit_reason\n";
        for (const auto& t : trades)
            f << t.entry_price << ',' << t.exit_price << ','
              << t.qty << ',' << t.side << ',' << t.pnl << ','
              << t.exit_reason << '\n';
        spdlog::info("[Backtest] trades saved to {}", path);
    }
};

// ── BacktestBar ──────────────────────────────────────────────────────────────────
// Input: CSV с полями date,open,high,low,close,volume
// Можно лоадить изторические D1 данные MOEX (формат Finam-экспорта)

struct BacktestBar {
    std::chrono::system_clock::time_point ts;
    double open, high, low, close;
    int64_t volume;
};

inline std::vector<BacktestBar> load_csv(const std::string& path) {
    std::vector<BacktestBar> bars;
    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::error("[Backtest] cannot open {}", path);
        return bars;
    }
    std::string line;
    std::getline(f, line); // skip header
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string tok;
        std::vector<std::string> cols;
        while (std::getline(ss, tok, ',')) cols.push_back(tok);
        if (cols.size() < 6) continue;
        BacktestBar b{};
        // date игнорируем ts — просто инкрементируем
        b.ts     = std::chrono::system_clock::now();
        b.open   = std::stod(cols[1]);
        b.high   = std::stod(cols[2]);
        b.low    = std::stod(cols[3]);
        b.close  = std::stod(cols[4]);
        b.volume = std::stoll(cols[5]);
        bars.push_back(b);
    }
    spdlog::info("[Backtest] loaded {} bars from {}", bars.size(), path);
    return bars;
}

// ── BacktestRunner ────────────────────────────────────────────────────────────────
//
// Прогоняет D1-бары через IStrategy::on_bar() + прогон цены через
// on_quote() (минимальная симуляция бар через open/close-квоты).
//
// Исполнение входа: по open следующего бара (slippage=0 по умолчанию).
// SL/TP: проверяем на каждом баре по high/low.

class BacktestRunner {
public:
    struct Config {
        double   tick_size   = 1.0;
        double   slippage    = 0.0;   // тики (0 = по open)
        double   commission  = 0.0;   // тики за сделку
    };

    explicit BacktestRunner(
        strategy::ConfluenceStrategy& strat,
        Config cfg = {})
        : strat_(strat), cfg_(cfg) {}

    BacktestResult run(const std::vector<BacktestBar>& bars) {
        BacktestResult res;
        double peak_pnl = 0.0;
        double cum_pnl  = 0.0;

        for (std::size_t i = 0; i < bars.size(); ++i) {
            const auto& bar = bars[i];
            feed_bar(bar, i);

            // Если в позиции — проверяем SL/TP по внутридневным движениям
            if (position_ != 0)
                check_intrabar_exit(bar, res, cum_pnl);

            // Доставляем close-квоту для обновления ORB/check_exit
            if (position_ == 0)
                try_entry_quote(bar, i, res);

            // Обновляем максимум drawdown
            peak_pnl = std::max(peak_pnl, cum_pnl);
            res.max_drawdown = std::max(res.max_drawdown, peak_pnl - cum_pnl);
        }
        compile_stats(res);
        return res;
    }

private:
    void feed_bar(const BacktestBar& b, std::size_t idx) {
        Bar bar{
            .symbol    = strat_.symbol(),
            .open      = b.open, .high = b.high,
            .low       = b.low,  .close = b.close,
            .volume    = b.volume,
            .ts        = b.ts,
            .timeframe = "D1",
        };
        strat_.on_bar(bar);
        (void)idx;
    }

    // Проверяем SL/TP по high/low бара
    void check_intrabar_exit(const BacktestBar& bar,
                              BacktestResult& res, double& cum_pnl)
    {
        if (position_ == 0 || entry_price_ < 1e-9) return;

        const double sl_price = (position_ > 0)
            ? entry_price_ - sl_ticks_ * cfg_.tick_size
            : entry_price_ + sl_ticks_ * cfg_.tick_size;
        const double tp_price = (position_ > 0)
            ? entry_price_ + tp_ticks_ * cfg_.tick_size
            : entry_price_ - tp_ticks_ * cfg_.tick_size;

        // Случай: SL и TP в одном баре — предполагаем SL (консервативно)
        const bool sl_hit = (position_ > 0) ? (bar.low  <= sl_price)
                                             : (bar.high >= sl_price);
        const bool tp_hit = (position_ > 0) ? (bar.high >= tp_price)
                                             : (bar.low  <= tp_price);

        if (sl_hit) {
            close_position(sl_price, "sl", bar.ts, res, cum_pnl);
        } else if (tp_hit) {
            close_position(tp_price, "tp", bar.ts, res, cum_pnl);
        }
    }

    void try_entry_quote(const BacktestBar& bar, std::size_t idx,
                          BacktestResult& res)
    {
        (void)res; (void)idx;
        // close квота — для check_exit и finalize_orb
        const double mid = bar.close;
        Quote q{
            .symbol = strat_.symbol(),
            .bid    = mid - 0.5, .ask = mid + 0.5,
            .ts     = bar.ts,
        };
        strat_.update_bbo(q.bid, q.ask);
        const auto sig = strat_.on_quote(q);
        if (sig.direction == Signal::Direction::None) return;
        if (sig.direction == Signal::Direction::Close) return; // позиции нет

        // Исполняем по open следующего бара (backtest-соглашение)
        pending_signal_ = sig;
    }

    void close_position(double exit_price, std::string_view reason,
                         std::chrono::system_clock::time_point ts,
                         BacktestResult& res, double& cum_pnl)
    {
        const double pnl_ticks = static_cast<double>(position_) *
            (exit_price - entry_price_) / cfg_.tick_size - cfg_.commission;
        cum_pnl += pnl_ticks;

        res.trades.push_back(TradeRecord{
            .entry_ts    = entry_ts_,
            .exit_ts     = ts,
            .entry_price = entry_price_,
            .exit_price  = exit_price,
            .qty         = std::abs(position_),
            .side        = (position_ > 0) ? 1 : -1,
            .pnl         = pnl_ticks,
            .exit_reason = std::string(reason),
        });

        // Симулируем on_order_update для стратегии
        OrderUpdate upd{
            .side       = (position_ > 0) ? OrderSide::Sell : OrderSide::Buy,
            .status     = OrderStatus::Filled,
            .price      = exit_price,
            .qty_filled = std::abs(position_),
        };
        strat_.on_order_update(upd);
        position_    = 0;
        entry_price_ = 0.0;
    }

    void compile_stats(BacktestResult& res) {
        res.total_trades = static_cast<int32_t>(res.trades.size());
        double gross_win = 0.0, gross_loss = 0.0;
        for (const auto& t : res.trades) {
            res.total_pnl += t.pnl;
            if (t.pnl > 0) { ++res.win_trades;  gross_win  += t.pnl; }
            else            { ++res.loss_trades; gross_loss -= t.pnl; }
        }
        if (res.total_trades > 0)
            res.win_rate = static_cast<double>(res.win_trades) / res.total_trades;
        if (res.win_trades  > 0) res.avg_win  = gross_win  / res.win_trades;
        if (res.loss_trades > 0) res.avg_loss = gross_loss / res.loss_trades;
        if (gross_loss > 1e-9)   res.profit_factor = gross_win / gross_loss;
    }

    strategy::ConfluenceStrategy& strat_;
    Config    cfg_;

    int32_t  position_{0};
    double   entry_price_{0.0};
    double   sl_ticks_{30.0};
    double   tp_ticks_{90.0};
    Signal   pending_signal_{};
    std::chrono::system_clock::time_point entry_ts_{};
};

} // namespace finam::backtest
