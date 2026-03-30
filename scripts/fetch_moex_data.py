#!/usr/bin/env python3
"""
fetch_moex_data.py — загрузка D1-баров фьючерсов FORTS через MOEX ISS API.

Использование:
    python3 scripts/fetch_moex_data.py --ticker Si --from 2024-01-01 --to 2026-03-30
    python3 scripts/fetch_moex_data.py --ticker RTS --from 2023-01-01

Выход: data/<TICKER>_D1.csv
Формат (BacktestRunner-совместимый):
    date,open,high,low,close,volume
Требует только stdlib: urllib, json, csv. Без зависимостей.
"""

import argparse
import csv
import json
import os
import time
import urllib.request
from datetime import date, datetime, timedelta
from pathlib import Path

# Квартальные серии FORTS: тикеры без серии (добавляется авто)
TICKER_BOARD = {
    "Si":   "RFUD",
    "RTS":  "RFUD",
    "GOLD": "RFUD",
    "MIX":  "RFUD",
}

# Базовые URL ISS API
ISS_CANDLES = "https://iss.moex.com/iss/engines/futures/markets/forts/securities/{sec}/candles.json"
ISS_SERIES  = "https://iss.moex.com/iss/engines/futures/markets/forts/securities.json"


def quarterly_series(ticker: str, from_date: date, to_date: date) -> list[str]:
    """
    Возвращает список квартальных серий (Si-3.24, Si-6.24, ...) для заданного диапазона.
    """
    series = []
    year, month = from_date.year, from_date.month
    # Доводим до ближайшего квартального
    while month % 3 != 0:
        month += 1
    while date(year, month, 1) <= to_date:
        yy = year % 100
        series.append(f"{ticker}-{month}.{yy:02d}")
        month += 3
        if month > 12:
            month -= 12
            year += 1
    return series


def fetch_candles(secid: str, from_date: date, to_date: date,
                  interval: int = 24) -> list[dict]:
    """
    Загружает D1 (или другой) interval для заданного инструмента.
    ISS возвращает макс. 500 баров за запрос, поэтому пагинируем.
    interval: 1=1m, 10=10m, 60=1h, 24=D1, 7=W1
    """
    bars: list[dict] = []
    start = 0
    url_base = ISS_CANDLES.format(sec=secid)

    while True:
        url = (f"{url_base}?interval={interval}"
               f"&from={from_date}&till={to_date}&start={start}")
        try:
            with urllib.request.urlopen(url, timeout=15) as resp:
                data = json.load(resp)
        except Exception as e:
            print(f"  [WARN] fetch failed for {secid} start={start}: {e}")
            break

        candles = data["candles"]
        columns = candles["columns"]  # ["open","close","high","low","value","volume","begin","end"]
        rows    = candles["data"]

        if not rows:
            break

        for row in rows:
            rec = dict(zip(columns, row))
            bars.append({
                "date":   rec["begin"][:10],
                "open":   rec["open"],
                "high":   rec["high"],
                "low":    rec["low"],
                "close":  rec["close"],
                "volume": int(rec["volume"]),
            })

        if len(rows) < 500:
            break  # последняя страница
        start += len(rows)
        time.sleep(0.3)  # rate-limit ISS: не более 3 req/s

    return bars


def merge_bars(all_bars: list[dict]) -> list[dict]:
    """
    Слияем бары нескольких контрактов, дедуплицируем и сортируем по дате.
    """
    seen: dict[str, dict] = {}
    for bar in all_bars:
        d = bar["date"]
        # если два контракта пересекаются по дате — берём максимальный volume
        if d not in seen or bar["volume"] > seen[d]["volume"]:
            seen[d] = bar
    return sorted(seen.values(), key=lambda x: x["date"])


def main():
    parser = argparse.ArgumentParser(description="Fetch MOEX FORTS futures D1 data")
    parser.add_argument("--ticker", default="Si", help="Base ticker (Si, RTS, GOLD)")
    parser.add_argument("--from",   dest="from_date",
                        default="2024-01-01", help="Start date YYYY-MM-DD")
    parser.add_argument("--to",     dest="to_date",
                        default=date.today().isoformat(), help="End date YYYY-MM-DD")
    parser.add_argument("--out",    default="data", help="Output directory")
    parser.add_argument("--interval", type=int, default=24,
                        help="ISS interval: 24=D1 (default), 60=H1, 10=M10")
    args = parser.parse_args()

    from_date = date.fromisoformat(args.from_date)
    to_date   = date.fromisoformat(args.to_date)
    ticker    = args.ticker

    Path(args.out).mkdir(parents=True, exist_ok=True)
    out_path = os.path.join(args.out, f"{ticker}_D1.csv")

    series = quarterly_series(ticker, from_date, to_date)
    print(f"Fetching {len(series)} contracts for {ticker}: {series}")

    all_bars: list[dict] = []
    for sec in series:
        print(f"  {sec} ...", end=" ", flush=True)
        bars = fetch_candles(sec, from_date, to_date, args.interval)
        print(f"{len(bars)} bars")
        all_bars.extend(bars)
        time.sleep(0.5)

    merged = merge_bars(all_bars)
    print(f"Total after merge: {len(merged)} bars")

    if not merged:
        print("[ERROR] no data fetched")
        return

    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["date","open","high","low","close","volume"])
        writer.writeheader()
        writer.writerows(merged)

    print(f"Saved to {out_path}")
    print(f"Date range: {merged[0]['date']} — {merged[-1]['date']}")


if __name__ == "__main__":
    main()
