"""
investigate_fills.py — Show trade history for a market to see how fast others acted.

Usage:
    python investigate_fills.py KXLOWTAUS-26APR02-B68.5
    python investigate_fills.py KXLOWTAUS-26APR02-B68.5 --limit 50
"""

import argparse
import asyncio
from datetime import datetime
from pathlib import Path

from dotenv import load_dotenv
load_dotenv(Path(__file__).parent / ".env")

from kalshi import KalshiClient


async def get_trades(client: KalshiClient, ticker: str, limit: int = 100) -> list[dict]:
    trades = []
    cursor = None
    while True:
        params = {"ticker": ticker, "limit": min(limit - len(trades), 100)}
        if cursor:
            params["cursor"] = cursor
        body   = await client._get("/markets/trades", params)
        chunk  = body.get("trades", [])
        trades.extend(chunk)
        cursor = body.get("cursor")
        if not cursor or not chunk or len(trades) >= limit:
            break
    return trades


async def get_my_orders(client: KalshiClient, ticker: str) -> list[dict]:
    body = await client._get("/portfolio/orders", {"ticker": ticker, "limit": 100})
    return body.get("orders", [])


def parse_ts(ts: str) -> datetime:
    return datetime.fromisoformat(ts.replace("Z", "+00:00"))


async def run(ticker: str, limit: int) -> None:
    client = KalshiClient()
    try:
        print(f"\nMarket: {ticker}")
        print("=" * 70)

        # ── My orders ─────────────────────────────────────────────────────────
        my_order_times = {}
        try:
            my_orders = await get_my_orders(client, ticker)
            if my_orders:
                print(f"\nMy orders ({len(my_orders)}):")
                for o in sorted(my_orders, key=lambda x: x.get("created_time", "")):
                    ts     = o.get("created_time", "")
                    status = o.get("status", "")
                    side   = o.get("side", "")
                    price  = o.get("no_price") or o.get("yes_price")
                    count  = o.get("count", 0)
                    filled = o.get("filled_count", 0)
                    oid    = o.get("order_id", "")
                    print(f"  {ts}  {side} x{count} @ {price}c  filled={filled}  status={status}  id={oid}")
                    my_order_times[oid] = parse_ts(ts) if ts else None
            else:
                print("\nNo orders found for this market.")
        except Exception as e:
            print(f"\n[!] Could not fetch my orders: {e}")

        # ── All trades ────────────────────────────────────────────────────────
        trades = await get_trades(client, ticker, limit)
        if not trades:
            print("\nNo trades found.")
            return

        print(f"\nAll trades ({len(trades)}) — chronological:")
        print(f"  {'time (UTC)':<32} {'side':<6} {'count':<7} {'no_price':<10} {'yes_price':<10} {'delta'}")
        print(f"  {'-'*32} {'-'*6} {'-'*7} {'-'*10} {'-'*10} {'-'*10}")

        trades_sorted = sorted(trades, key=lambda x: x.get("created_time", ""))
        first_ts = None
        for t in trades_sorted:
            ts_str    = t.get("created_time", "")
            ts        = parse_ts(ts_str) if ts_str else None
            if first_ts is None and ts:
                first_ts = ts
            delta     = f"+{(ts - first_ts).total_seconds():.1f}s" if ts and first_ts else ""
            side      = t.get("taker_side", "?")
            count     = t.get("count_fp", t.get("count", "?"))
            no_price  = t.get("no_price_dollars", "?")
            yes_price = t.get("yes_price_dollars", "?")
            print(f"  {ts_str:<32} {side:<6} {str(count):<7} {str(no_price):<10} {str(yes_price):<10} {delta}")

        # ── Speed comparison ───────────────────────────────────────────────────
        if my_order_times and trades_sorted and first_ts:
            print(f"\nFirst trade on market: {first_ts.isoformat()}")
            for oid, my_ts in my_order_times.items():
                if my_ts:
                    lag = (my_ts - first_ts).total_seconds()
                    print(f"My order {oid[:8]}...  placed: {my_ts.isoformat()}  lag={lag:+.1f}s")
    finally:
        await client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ticker", help="Market ticker, e.g. KXLOWTAUS-26APR02-B68.5")
    parser.add_argument("--limit", type=int, default=100)
    args = parser.parse_args()
    asyncio.run(run(args.ticker.upper(), args.limit))


if __name__ == "__main__":
    main()
