# ── tests/CMakeLists.txt ───────────────────────────────────────────────────────────────
#
# Структура:
#   tests/
#     unit/           — быстрые тесты без I/O (чистая логика)
#     backtest/       — прогон баров через IStrategy, сбор статистики

add_subdirectory(unit)
add_subdirectory(backtest)
