"""
trader.py — Event-driven Kalshi weather trader.

Usage:
    python trader.py

Receives temperature pushes from Synoptic (~1-minute resolution).
On each new observation, checks the markets for that station and places
NO orders on any bracket that the current temperature has made impossible.

Add new cities to STATION_MARKETS below.
"""

import asyncio
import json
import logging
import os
import time
from datetime import datetime, date
from pathlib import Path
from zoneinfo import ZoneInfo

from dotenv import load_dotenv

load_dotenv(Path(__file__).parent / ".env")

import weather as W
from kalshi import KalshiClient
from logic import no_targets

# ── Config ────────────────────────────────────────────────────────────────────

SYNOPTIC_TOKEN   = os.environ.get("SYNOPTIC_TOKEN", "8bd5bb0cd3cc44ac8b65d940dd9fbd9e")

# Each entry: stid → {low_series, high_series}  (None = not trading that side)
STATION_MARKETS = {
    "KMDW1M": {"low": "KXLOWTCHI",  "high": "KXHIGHCHI"},
    "KLAX1M": {"low": "KXLOWTLAX",  "high": "KXHIGHLAX"},
    "KMIA1M": {"low": "KXLOWTMIA",  "high": "KXHIGHMIA"},
    "KDEN1M": {"low": "KXLOWTDEN",  "high": "KXHIGHDEN"},
    "KAUS1M": {"low": "KXLOWTAUS",  "high": "KXHIGHAUS"},
    "KPHL1M": {"low": "KXLOWTPHIL", "high": "KXHIGHPHIL"},
    "KDCA1M": {"low": None,          "high": "KXHIGHTDC"},
    "KATL1M": {"low": None,          "high": "KXHIGHTATL"},
    "KOKC1M": {"low": None,          "high": "KXHIGHTOKC"},
    "KSAT1M": {"low": None,          "high": "KXHIGHTSATX"},
    "KMSY1M": {"low": None,          "high": "KXHIGHTNOLA"},
    "KLAS1M": {"low": None,          "high": "KXHIGHTLV"},
    "KMSP1M": {"low": None,          "high": "KXHIGHTMIN"},
    "KSFO1M": {"low": None,          "high": "KXHIGHTSFO"},
    "KJFK1M": {"low": "KXLOWTNYC",  "high": None},
    "KDFW1M": {"low": None,          "high": "KXHIGHTDAL"},
    "KHOU1M": {"low": None,          "high": "KXHIGHTHOU"},
}

ORDER_SIZE       = 25     # contracts per order
MIN_YES_PRICE    = 5     # skip if YES already below this (cents)
MAX_NO_PRICE     = 99    # never pay more than this for a NO contract (cents)
MARKET_CACHE_TTL = 300   # seconds before re-fetching market list

LOCAL_TZ         = ZoneInfo("America/Chicago")
HERE             = Path(__file__).parent
TRADED_FILE      = HERE / "traded.json"
LOG_DIR          = HERE / "logs"


# ── Logging ───────────────────────────────────────────────────────────────────

def _setup_logging() -> logging.Logger:
    LOG_DIR.mkdir(exist_ok=True)
    log_file = LOG_DIR / f"trader-{date.today()}.log"
    fmt = "%(asctime)s  %(levelname)-7s  %(message)s"
    import sys
    logging.basicConfig(
        level=logging.INFO,
        format=fmt,
        handlers=[
            logging.FileHandler(log_file, encoding="utf-8"),
            logging.StreamHandler(stream=open(sys.stdout.fileno(), mode='w', encoding='utf-8', closefd=False)),
        ],
    )
    return logging.getLogger("trader")

log = _setup_logging()


# ── Traded persistence ────────────────────────────────────────────────────────

def _load_traded() -> set[str]:
    """Load today's already-traded tickers from disk."""
    today = str(date.today())
    if TRADED_FILE.exists():
        try:
            data = json.loads(TRADED_FILE.read_text())
            if data.get("date") == today:
                return set(data.get("tickers", []))
        except Exception:
            pass
    return set()

def _save_traded(traded: set[str]) -> None:
    today = str(date.today())
    TRADED_FILE.write_text(json.dumps({"date": today, "tickers": sorted(traded)}, indent=2))


# ── State ─────────────────────────────────────────────────────────────────────

class TraderState:
    def __init__(self, client: KalshiClient):
        self.client = client
        self.traded: set[str] = _load_traded()
        self._market_cache: dict[str, list] = {}
        self._cache_ts:     dict[str, float] = {}
        self._last_bounds:  dict[str, tuple] = {}   # stid -> (f_floor, f_ceil)
        if self.traded:
            log.info("Loaded %d already-traded tickers from today: %s", len(self.traded), sorted(self.traded))

    def _suffix(self) -> str:
        return datetime.now(LOCAL_TZ).strftime("%y%b%d").upper()

    async def _get_markets(self, event_ticker: str) -> list[dict]:
        now = time.monotonic()
        if (event_ticker not in self._market_cache or
                now - self._cache_ts.get(event_ticker, 0) > MARKET_CACHE_TTL):
            self._market_cache[event_ticker] = await self.client.get_markets(event_ticker)
            self._cache_ts[event_ticker]     = now
        return self._market_cache[event_ticker]

    async def on_temp(self, stid: str, raw_f: float, f_floor: int, f_ceil: int, obs_time: str) -> None:
        if stid not in STATION_MARKETS:
            return

        log.info("%s  %.1fF  floor=%dF  ceil=%dF  obs=%s", stid, raw_f, f_floor, f_ceil, obs_time)

        # Skip if bounds haven't changed for this station
        if (f_floor, f_ceil) == self._last_bounds.get(stid):
            return
        self._last_bounds[stid] = (f_floor, f_ceil)

        cfg    = STATION_MARKETS[stid]
        sfx    = self._suffix()
        all_targets = []

        for side, series in [("low", cfg["low"]), ("high", cfg["high"])]:
            if not series:
                continue
            event = f"{series}-{sfx}"
            try:
                markets = await self._get_markets(event)
            except Exception as e:
                log.error("%s fetch failed: %s", event, e)
                continue
            targets = no_targets(f_floor, f_ceil, markets, side,
                                 min_yes=MIN_YES_PRICE, traded_set=self.traded)
            all_targets.extend((side.upper(), m) for m in targets)

        if not all_targets:
            return

        for side_label, mkt in all_targets:
            ticker   = mkt["ticker"]
            no_price = min(mkt["no_price"], MAX_NO_PRICE)
            yes_ask  = mkt["yes_price"]
            log.info(
                "  -> BUY NO [%s] %s  [%s,%s)F  YES=%dc  NO=%dc  x%d  "
                "(trigger: floor=%d ceil=%d)",
                side_label, ticker,
                mkt.get('floor_strike'), mkt.get('cap_strike'),
                yes_ask, no_price, ORDER_SIZE,
                f_floor, f_ceil,
            )
            try:
                order = await self.client.place_no_order(ticker, ORDER_SIZE, no_price)
                log.info("     order_id=%s  status=%s", order.get('order_id'), order.get('status'))
                self.traded.add(ticker)
                _save_traded(self.traded)
            except Exception as e:
                log.error("     order failed: %s", e)


# ── Entry point ───────────────────────────────────────────────────────────────

async def _prefetch_markets(state: TraderState) -> None:
    """Warm the market cache for every configured series before the stream starts."""
    sfx   = state._suffix()
    total = 0
    for stid, cfg in STATION_MARKETS.items():
        for side, series in [("low", cfg["low"]), ("high", cfg["high"])]:
            if not series:
                continue
            event = f"{series}-{sfx}"
            try:
                mkts = await state._get_markets(event)
                log.info("  prefetch %s: %d markets", event, len(mkts))
                total += 1
            except Exception as e:
                log.error("  prefetch %s failed: %s", event, e)
            await asyncio.sleep(0.25)   # 4 req/s — well under Kalshi rate limit
    log.info("Prefetch complete (%d events cached)", total)


async def main():
    client = KalshiClient()
    try:
        state = TraderState(client)
        stids = list(STATION_MARKETS.keys())
        log.info("Started — watching %s", stids)
        await _prefetch_markets(state)
        await W.stream_temp(SYNOPTIC_TOKEN, stids, state.on_temp)
    finally:
        await client.close()


if __name__ == "__main__":
    asyncio.run(main())
