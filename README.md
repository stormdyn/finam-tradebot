# finam-tradebot

Алготрейдинговый бот для MOEX FORTS (фьючерсы Si, RTS, GOLD и другие) через брокера Финам. Написан на C++23, подключается к [Finam Trade API v2](https://tradeapi.finam.ru) по gRPC/TLS.

## Стратегия

**Confluence** — сигнал на вход только при совпадении трёх независимых компонентов:

| Компонент | Что измеряет | Порог входа |
|-----------|-------------|-------------|
| **ORB** (Opening Range Breakout) | Пробой диапазона первых 15 минут сессии | Цена выше/ниже ORB |
| **MLOFI** (Multi-Level Order Flow Imbalance) | Дисбаланс стакана по 5 уровням с экспоненциальным весом | `score > 3.0` |
| **TFI** (Trade Flow Imbalance) | CVD + скорость сделок за 5-секундное окно | `|tfi| > 0.3` |

Дополнительные фильтры:
- **NR7** — наименьший дневной диапазон за 7 дней → размер позиции ×1.0 (иначе ×0.5)
- **ATR-фильтр** — не торгуем если дневной диапазон < 0.8 × ATR(14)
- **SpoofFilter** — блокирует вход если cancel_ratio > 80% на ближних уровнях стакана
- **RiskManager** — ГО из API, дневной стоп-лосс 5%, drawdown 15%, max 3 позиции (суммарно по всем инструментам)

**SL: 30 тиков, TP: 90 тиков** (соотношение 1:3).

## Архитектура

```
gRPC streams (MD)
  SubscribeOrderBook ──► BookLevelEvent ──► SPSC Queue ──► StrategyRunner[Si]
  SubscribeLatestTrades ► TradeEvent   ──►               ──► ConfluenceStrategy
  SubscribeBars (M1/D1) ► Bar          ──► (direct)           ├── OrderBookState (MLOFI)
  SubscribeQuotes ──────► Quote         ──►                    ├── TradeFlowAnalyzer (TFI)
                                                               ├── SessionContext (ORB/NR7)
                                                               └── SpoofFilter

  Аналогично: StrategyRunner[RTS], StrategyRunner[GOLD] — каждый независим

OrderClient (SubscribeOrders) ──► broadcast ──► каждый Runner фильтрует по символу
RiskManager (общий) ── limit: 3 позиции суммарно по всем инструментам
```

**Мультисимвольность**: один `StrategyRunner` = один инструмент. N инструментов → N раннеров. Каждый имеет свою стратегию, SPSC-очередь и потоки. `OrderClient` и `RiskManager` — общие.

**JWT**: фоновый поток держит `SubscribeJwtRenewal` стрим, обновляет токен атомарно.

## Установка зависимостей

### Arch Linux

```bash
# Основные зависимости из официальных репозиториев
sudo pacman -S base-devel cmake ninja python-pip

# gRPC + protobuf
sudo pacman -S grpc protobuf

# Conan 2
pip install --user 'conan==2.*'
# или через pipx (рекомендуется)
pipx install 'conan==2.*'

conan profile detect --force
```

> **Arch note**: grpc из `[extra]` включает `grpc_cpp_plugin` и `libgrpc++`. Версия может отличаться от Ubuntu — если CMake не находит gRPC, добавьте `-DCMAKE_PREFIX_PATH=/usr` при configure.

### Ubuntu 24.04

```bash
sudo apt-get install -y build-essential cmake ninja-build \
  libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc

pip3 install 'conan==2.*'
conan profile detect --force
```

## Сборка

```bash
git clone --recursive https://github.com/stormdyn/finam-tradebot
cd finam-tradebot

# Установить зависимости через Conan (spdlog, nlohmann_json, catch2)
conan install . --output-folder=build --build=missing \
  -s compiler.cppstd=23 -s build_type=Release

# Arch: если используется clang вместо gcc
# conan install . --output-folder=build --build=missing \
#   -s compiler=clang -s compiler.cppstd=23 -s build_type=Release

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake \
  -DCMAKE_CXX_STANDARD=23

cmake --build build --parallel $(nproc)
```

Бинарники после сборки:
- `build/finam_tradebot` — основной бот
- `build/connectivity_check` — проверка подключения к API
- `build/unit_tests` — юнит-тесты
- `build/backtest_runner` — бэктест на исторических данных

## Проверка подключения

Перед первым запуском убедитесь что токен работает:

```bash
export FINAM_SECRET_TOKEN=ваш_токен
./build/connectivity_check
```

Ожидаемый вывод:
```
[1/4] OK: JWT obtained
[2/4] OK: account_id=A12345
[3/4] OK
[4/4] OK
All checks passed. Bot is ready to start.
```

## Запуск

```bash
# Dry-run — ордера не отправляются, только логирование сигналов
export FINAM_SECRET_TOKEN=ваш_токен
./build/finam_tradebot --dry-run --debug

# Боевой запуск
export FINAM_SECRET_TOKEN=ваш_токен
./build/finam_tradebot
```

Health check: `http://localhost:8080/health` | Метрики: `http://localhost:8080/metrics`

### Docker

```bash
docker build -t finam-tradebot:latest .

# Dry-run
FINAM_SECRET_TOKEN=ваш_токен docker compose run --rm tradebot --dry-run

# Боевой
FINAM_SECRET_TOKEN=ваш_токен docker compose up -d
docker compose logs -f tradebot
```

## Настройка торговых инструментов

Список символов задаётся в `src/main.cpp` в функции `make_symbol_configs()`:

```cpp
static std::vector<SymbolConfig> make_symbol_configs() {
    return {
        // ticker   tick   qty  sl    tp    roll
        { "Si",    1.0,    1,  30.0, 90.0,  5 },   // USD/RUB фьючерс
        { "RTS",   10.0,   1,  30.0, 90.0,  5 },   // индекс RTS
        { "GOLD",  1.0,    1,  30.0, 90.0,  5 },   // золото
        // { "MIX",  0.05,  1,  30.0, 90.0,  5 },  // раскомментировать для индекса МосБиржи
    };
}
```

После изменения — пересобрать (`cmake --build build`). Нет runtime-конфигурации намеренно: статический список безопаснее и проще отлаживать.

| Поле | Описание |
|------|---------|
| `ticker` | Тикер без серии: `"Si"`, `"RTS"`, `"GOLD"`, `"MIX"` |
| `tick_size` | Размер тика в рублях (Si=1, RTS=10, GOLD=1) |
| `base_qty` | Контрактов на сигнал (без NR7 фактически × 0.5 = 0) |
| `sl_ticks` | Стоп-лосс в тиках |
| `tp_ticks` | Тейк-профит в тиках |
| `rollover_days` | За сколько дней до экспирации переходить на следующий контракт |

**Лимит позиций** (`max_positions = 3`) — суммарно по всем инструментам. При 3 символах может быть открыта одна позиция по каждому, или 3 по одному.

## Торговые параметры

| Параметр | Дефолт | Описание |
|----------|--------|----------|
| `per_trade_pct` | 2.0% | Макс. ГО от капитала на одну сделку |
| `max_daily_loss_pct` | 5.0% | Дневной стоп (circuit breaker) |
| `max_drawdown_pct` | 15.0% | Максимальная просадка от пика |
| `max_positions` | 3 | Макс. открытых позиций (все символы) |
| `history_days` | 20 | Дней D1 истории для ATR/NR7 при старте |

## Символы FORTS

Формат: `{TICKER}-{MM}.{YY}@FORTS`

```
Si-6.26@FORTS   — USD/RUB, июнь 2026
RTS-6.26@FORTS  — индекс RTS
GOLD-6.26@FORTS — золото
MIX-6.26@FORTS  — индекс МосБиржи
```

Бот **автоматически выбирает ближайший контракт** и роллирует за `rollover_days` дней до экспирации (третья пятница квартального месяца: март, июнь, сентябрь, декабрь).

## Торговые сессии FORTS

| Сессия | Время MSK |
|--------|-----------|
| Основная | 10:00 – 18:45 |
| Вечерняя | 19:00 – 23:50 |
| Техобслуживание | 05:00 – 06:15 (бот ждёт автоматически) |

ORB формируется в первые 15 минут основной сессии (10:00–10:15 MSK).

## Токен Finam Trade API

Токен генерируется на [tradeapi.finam.ru](https://tradeapi.finam.ru) → «API-ключи». Передаётся **только через переменную среды**:

```bash
export FINAM_SECRET_TOKEN=ваш_токен
```

Никогда не коммитьте токен, не передавайте через `--build-arg` Docker, не пишите в логах. Подробнее: [docs/secrets.md](docs/secrets.md).

## Бэктест

```bash
python3 scripts/fetch_moex_data.py Si 2025-01-01 2026-01-01 > data/Si_D1.csv

# sl=30 тиков, tp=90 тиков, комиссия=0.5 тиков
./build/backtest_runner data/Si_D1.csv 30 90 0.5
```

## Стек

| Компонент | Технология |
|-----------|-----------|
| Язык | C++23 (`std::expected`, concepts, ranges) |
| gRPC | `grpc++` + protobuf, TLS → `api.finam.ru:443` |
| Логирование | spdlog (rotating file + stdout, 5×10 MB) |
| Сборка | CMake 3.25+ + Conan 2 |
| Контейнер | Docker multi-stage (builder → ubuntu:24.04 runtime) |
| CI | GitHub Actions (build + unit tests) |
