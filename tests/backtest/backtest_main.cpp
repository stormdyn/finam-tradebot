#include <iostream>
#include <string>
#include <spdlog/spdlog.h>
#include "backtest_runner.hpp"

// ─────────────────────────────────────────────────────────────────────────
// Использование:
//   ./backtest_runner <csv_path> [sl_ticks] [tp_ticks] [commission]
//
// Пример:
//   ./backtest_runner data/Si-6.26_D1.csv 30 90 0.5
//
// CSV-формат (заголовок):
//   date,open,high,low,close,volume
// ─────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S.%e] %v");

    if (argc < 2) {
        std::cerr << "Usage: backtest_runner <csv> [sl] [tp] [commission]\n";
        return 1;
    }

    const std::string csv_path = argv[1];
    const double sl_ticks  = (argc > 2) ? std::stod(argv[2]) : 30.0;
    const double tp_ticks  = (argc > 3) ? std::stod(argv[3]) : 90.0;
    const double commission = (argc > 4) ? std::stod(argv[4]) : 0.0;

    spdlog::info("[Backtest] file={} sl={} tp={} comm={}",
        csv_path, sl_ticks, tp_ticks, commission);

    // Настраиваем стратегию
    finam::strategy::ConfluenceStrategy strat{{
        .symbol    = finam::Symbol{"Si-6.26", "FORTS"},
        .base_qty  = 1,
        .sl_ticks  = sl_ticks,
        .tp_ticks  = tp_ticks,
        .tick_size = 1.0,
    }};

    finam::backtest::BacktestRunner runner{strat,
        finam::backtest::BacktestRunner::Config{
            .tick_size  = 1.0,
            .slippage   = 0.0,
            .commission = commission,
        }
    };

    const auto bars   = finam::backtest::load_csv(csv_path);
    const auto result = runner.run(bars);
    result.print();
    result.to_csv(csv_path + ".trades.csv");
    return 0;
}
