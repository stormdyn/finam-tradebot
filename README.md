# finam-tradebot

Алготрейдинговый бот для MOEX FORTS (фьючерсы Si, RTS, GOLD и другие) через брокера Финам. Написан на C++23, подключается к [Finam Trade API v2](https://tradeapi.finam.ru) по gRPC/TLS.

## Стратегия

**Confluence** — комбинация трёх независимых сигналов, сигнал на вход только при совпадении всех трёх:

| Компонент | Что измеряет | Порог входа |
|-----------|-------------|-------------|
| **ORB** (Opening Range Breakout) | Пробой диапазона первых 15 минут сессии | Цена выше/ниже ORB |
| **MLOFI** (Multi-Level Order Flow Imbalance) | Дисбаланс стакана по 5 уровням с экспоненциальным весом | `score > 3.0` |
| **TFI** (Trade Flow Imbalance) | CVD + скорость сделок за 5-секундное окно | `|tfi| > 0.3` |

Дополнительные фильтры:
- **NR7** — если текущий день имеет наименьший диапазон за 7 дней, размер позиции ×1.0 вместо 0.5
- **ATR-фильтр** — не торгуем если дневной диапазон < 0.8 × ATR(14)
- **SpoofFilter** — блокирует вход если cancel_ratio > 80% на ближних уровнях стакана
- **RiskManager** — ГО из API, дневной стоп-лосс 5%, drawdown 15%, max 3 позиции

**SL: 30 тиков, TP: 90 тиков** (соотношение 1:3).

## Архитектура

```
gRPC streams (MD)
  SubscribeOrderBook ──► BookLevelEvent ──► SPSC Queue ──► StrategyRunner
  SubscribeLatestTrades ► TradeEvent   ──►               ──► ConfluenceStrategy
  SubscribeBars (M1/D1) ► Bar          ──► (direct)           ├── OrderBookState (MLOFI)
  SubscribeQuotes ──────► Quote         ──►                    ├── TradeFlowAnalyzer (TFI)
                                                               ├── SessionContext (ORB/NR7/ATR)
OrderClient (SubscribeOrders stream) ──────────────────────► on_order_update()
                                                               └── SpoofFilter
RolloverThread (каждую минуту) ──────────────────────────────► nearest_contract()
```

**Threading**: MD-потоки (gRPC) пишут в lock-free SPSC-очередь (1024 событий), strategy thread читает. Нет мьютексов на hot-path.

**JWT**: фоновый поток держит `SubscribeJwtRenewal` стрим, обновляет токен атомарно без остановки бота.

## Быстрый старт

### Зависимости

```bash
# Ubuntu 24.04
sudo apt-get install -y build-essential cmake ninja-build \
  libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc

pip3 install 'conan==2.*'
conan profile detect --force
```

### Сборка

```bash
git clone --recursive https://github.com/stormdyn/finam-tradebot
cd finam-tradebot

# Установить зависимости через Conan
conan install . --output-folder=build --build=missing \
  -s compiler.cppstd=23 -s build_type=Release

# Сконфигурировать и собрать
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

### Проверка подключения

Перед первым запуском убедитесь что токен работает и ГО доступно:

```bash
export FINAM_SECRET_TOKEN=ваш_токен
./build/connectivity_check
# Ожидаемый вывод:
# [1/4] OK: JWT obtained
# [2/4] OK: account_id=A12345
# [3/4] OK
# [4/4] OK
# All checks passed. Bot is ready to start.
```

### Запуск

```bash
# Dry-run — ордера не отправляются на биржу, только логирование
export FINAM_SECRET_TOKEN=ваш_токен
./build/finam_tradebot --dry-run --debug

# Боевой запуск
export FINAM_SECRET_TOKEN=ваш_токен
./build/finam_tradebot
```

Health check доступен на `http://localhost:8080/health` и `http://localhost:8080/metrics` (Prometheus формат).

### Docker

```bash
# Сборка образа
docker build -t finam-tradebot:latest .

# Dry-run
FINAM_SECRET_TOKEN=ваш_токен docker compose run --rm tradebot --dry-run

# Боевой запуск
FINAM_SECRET_TOKEN=ваш_токен docker compose up -d

# Логи
docker compose logs -f tradebot

# Health check
curl http://localhost:8080/health
```

### Бэктест

Скачайте исторические данные (встроенный скрипт):

```bash
python3 scripts/fetch_moex_data.py Si 2025-01-01 2026-01-01 > data/Si_D1.csv

# Запуск бэктеста: sl=30 тиков, tp=90 тиков, комиссия=0.5 тиков
./build/backtest_runner data/Si_D1.csv 30 90 0.5
```

## Конфигурация

Все параметры задаются в `src/main.cpp` (пересборка не требует перезапуска системы):

| Параметр | Дефолт | Описание |
|----------|--------|----------|
| `base_qty` | 1 | Базовый размер позиции в контрактах |
| `sl_ticks` | 30 | Стоп-лосс в тиках |
| `tp_ticks` | 90 | Тейк-профит в тиках |
| `tick_size` | 1.0 | Размер тика (Si = 1 руб.) |
| `per_trade_pct` | 2.0 | Макс. % ГО от капитала на сделку |
| `max_daily_loss_pct` | 5.0 | Дневной стоп (% от капитала) |
| `max_drawdown_pct` | 15.0 | Максимальная просадка |
| `max_positions` | 3 | Максимум одновременных позиций |
| `rollover_days` | 5 | За сколько дней до экспирации роллировать |
| `history_days` | 20 | Дней истории D1 для ATR/NR7 при старте |

## Торговые символы

Формат: `{TICKER}-{MM}.{YY}@FORTS`

```
Si-6.26@FORTS   — фьючерс на USD/RUB, июнь 2026
RTS-6.26@FORTS  — фьючерс на индекс RTS
GOLD-6.26@FORTS — фьючерс на золото
```

Бот **автоматически выбирает ближайший контракт** (`nearest_contract()`) и роллирует за `rollover_days` дней до экспирации (третья пятница квартального месяца).

## Торговые сессии FORTS

| Сессия | Время MSK |
|--------|-----------|
| Основная | 10:00 – 18:45 |
| Вечерняя | 19:00 – 23:50 |
| Техобслуживание | 05:00 – 06:15 (бот ждёт автоматически) |

ORB формируется в первые 15 минут основной сессии (10:00–10:15 MSK).

## Токен Finam Trade API

Токен генерируется на [tradeapi.finam.ru](https://tradeapi.finam.ru) → Апи-ключи. Передаётся **только через переменную среды**:

```bash
export FINAM_SECRET_TOKEN=ваш_токен
```

Никогда не коммитьте токен в репозиторий, не передавайте через `--build-arg` Docker, не пишите в логах. Подробнее: [docs/secrets.md](docs/secrets.md).

## Стек

| Компонент | Технология |
|-----------|-----------|
| Язык | C++23 (concepts, `std::expected`, ranges) |
| gRPC | `grpc++` + protobuf, TLS к `api.finam.ru:443` |
| Логирование | spdlog (rotating file + stdout, 5×10MB) |
| Сборка | CMake 3.25+ + Conan 2 |
| Контейнер | Docker multi-stage (builder → ubuntu:24.04 runtime) |
| CI | GitHub Actions (build + unit tests) |
| Конфигурация | JSON (`nlohmann_json`) |
