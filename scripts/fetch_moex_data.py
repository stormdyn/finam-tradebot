#!/usr/bin/env python3
"""
fetch_moex_data.py — загрузка D1-баров фьючерсов FORTS через MOEX ISS API.

MOEX ISS использует CME-style secid:
  H = март, M = июнь, U = сентябрь, Z = декабрь
  Пример: Si-6.24 → SiM4, Si-3.26 → SiH6, RTS-12.25 → RTSZ5

Использование:
    python3 scripts/fetch_moex_data.py --ticker Si --from 2024-01-01
    python3 scripts/fetch_moex_data.py --ticker RTS --from 2023-01-01 --to 2026-03-30

Выход: data/<TICKER>_D1.csv  (формат BacktestRunner)
    date,open,high,low,close,volume

Проверка secid в браузере (без авторизации):
    https://iss.moex.com/iss/history/engines/futures/markets/forts/securities/SiM6/candles.json?interval=24&from=2026-01-01&till=2026-03-30&iss.meta=off

Только stdlib — ничего устанавливать не нужно.
"""

import argparse
import csv
import json
import os
import time
import urllib.request
from datetime import date
from pathlib import Path

# Месяц → CME-литера
MONTH_LETTER = {3: "H", 6: "M", 9: "U", 12: "Z"}

# ISS history endpoint (содержит OHLCV по дням, в отличие от /securities/{}/candles)
ISS_URL = (
    "https://iss.moex.com/iss/history/engines/futures/markets/forts"
    "/securities/{sec}/candles.json"
    "?interval={interval}&from={from_}&till={till}&start={start}&iss.meta=off"
)


def make_secid(ticker: str, month: int, year: int) -> str:
    """Si, 6, 2024 → SiM4"""
    return f"{ticker}{MONTH_LETTER[month]}{year % 100}"


def quarterly_series(ticker: str, from_date: date, to_date: date) -> list[tuple[str, int, int]]:
    """
    Все квартальные контракты в диапазоне [from_date, to_date].
    Возвращает [(secid, month, year), ...].
    """
    series = []
    year, month = from_date.year, from_date.month
    # Доводим до ближайшего квартального
    while month % 3 != 0:
        month += 1
        if month > 12:
            month = 3
            year += 1
    while date(year, month, 1) <= to_date:
        series.append((make_secid(ticker, month, year), month, year))
        month += 3
        if month > 12:
            month -= 12
            year += 1
    return series


def fetch_candles(secid: str, from_date: date, to_date: date,
                  interval: int = 24) -> list[dict]:
    """
    Загружает все бары с пагинацией (макс 100 за запрос в ISS history API).
    """
    bars: list[dict] = []
    start = 0

    while True:
        url = ISS_URL.format(
            sec=secid, interval=interval,
            from_=from_date, till=to_date, start=start
        )
        try:
            req = urllib.request.Request(
                url, headers={"Accept": "application/json",
                               "User-Agent": "finam-tradebot-backtest/1.0"}
            )
            with urllib.request.urlopen(req, timeout=15) as resp:
                data = json.load(resp)
        except Exception as e:
            print(f"  [WARN] {secid} start={start}: {e}")
            break

        candles = data.get("candles", {})
        columns = candles.get("columns", [])
        rows    = candles.get("data", [])

        if not rows:
            break

        for row in rows:
            rec = dict(zip(columns, row))
            try:
                bars.append({
                    "date":   str(rec["begin"])[:10],
                    "open":   float(rec["open"]),
                    "high":   float(rec["high"]),
                    "low":    float(rec["low"]),
                    "close":  float(rec["close"]),
                    "volume": int(float(rec["volume"])),
                })
            except (KeyError, ValueError, TypeError):
                continue

        if len(rows) < 100:
            break  # последняя страница
        start += len(rows)
        time.sleep(0.3)  # не более 3 req/s

    return bars


def merge_bars(all_bars: list[dict]) -> list[dict]:
    """
    Дедупликация по дате: в день перекрытия двух контрактов
    берём тот, у которого больше volume (ближний контракт ликвиднее).
    """
    seen: dict[str, dict] = {}
    for bar in all_bars:
        d = bar["date"]
        if d not in seen or bar["volume"] > seen[d]["volume"]:
            seen[d] = bar
    return sorted(seen.values(), key=lambda x: x["date"])


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Fetch MOEX FORTS futures D1 data via ISS history API"
    )
    parser.add_argument("--ticker",   default="Si",
                        help="Base ticker: Si, RTS, GOLD, MIX")
    parser.add_argument("--from",     dest="from_date",
                        default="2024-01-01", help="Start YYYY-MM-DD")
    parser.add_argument("--to",       dest="to_date",
                        default=date.today().isoformat(), help="End YYYY-MM-DD")
    parser.add_argument("--out",      default="data",  help="Output directory")
    parser.add_argument("--interval", type=int, default=24,
                        help="ISS interval: 24=D1 (default), 60=H1")
    args = parser.parse_args()

    from_date = date.fromisoformat(args.from_date)
    to_date   = date.fromisoformat(args.to_date)
    ticker    = args.ticker

    Path(args.out).mkdir(parents=True, exist_ok=True)
    out_path  = os.path.join(args.out, f"{ticker}_D1.csv")

    series = quarterly_series(ticker, from_date, to_date)
    print(f"Fetching {len(series)} contracts for {ticker}:")
    print(f"  {[s[0] for s in series]}")
    print(f"  Проверка в браузере:")
    sample = make_secid(ticker, 6, to_date.year)
    print(f"  https://iss.moex.com/iss/history/engines/futures/markets/forts"
          f"/securities/{sample}/candles.json?interval=24"
          f"&from={from_date}&till={to_date}&iss.meta=off")
    print()

    all_bars: list[dict] = []
    for secid, _, _ in series:
        print(f"  {secid} ...", end=" ", flush=True)
        bars = fetch_candles(secid, from_date, to_date, args.interval)
        print(f"{len(bars)} bars")
        all_bars.extend(bars)
        time.sleep(0.5)

    merged = merge_bars(all_bars)
    print(f"\nTotal after merge: {len(merged)} bars")

    if not merged:
        print("[ERROR] No data fetched.")
        print("Проверь secid в браузере (см. ссылку выше) — возможно ISS изменил формат.")
        return

    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(
            f, fieldnames=["date", "open", "high", "low", "close", "volume"]
        )
        writer.writeheader()
        writer.writerows(merged)

    print(f"Saved → {out_path}")
    print(f"Range: {merged[0]['date']} — {merged[-1]['date']}")
    print(f"\nЗапуск бэктеста:")
    print(f"  ./build/tests/backtest/backtest_runner {out_path} 30 90 0.5")


if __name__ == "__main__":
    main()
