"""
weather.py — Synoptic push stream for real-time temperature.

Connects to wss://push.synopticdata.com and calls on_temp(raw_f, f_floor, f_ceil, obs_time)
within seconds of each new observation from KMDW1M (~1-minute resolution).

C→F rounding — why two bounds matter
──────────────────────────────────────
ASOS stations record in whole °F, but transmit in whole °C. The roundtrip
introduces 1-2°F ambiguity. asos_lookup_table.csv is the precomputed table
of every integer °C and its min/max original °F range.

For trading:
  f_floor = actual_range_min  →  day's max is GUARANTEED ≥ f_floor  (HIGH market)
  f_ceil  = actual_range_max  →  day's min is GUARANTEED ≤ f_ceil   (LOW  market)

Using f_floor for HIGH and f_ceil for LOW ensures we never trade a bracket
that is only arguably impossible due to rounding.
"""

import asyncio
import csv
import json
import logging
from pathlib import Path
from typing import Awaitable, Callable

import websockets

log = logging.getLogger("trader")

PUSH_HOST  = "push.synopticdata.com"
_TABLE_CSV = Path(__file__).parent / "asos_lookup_table.csv"


# ── Load lookup table ─────────────────────────────────────────────────────────

def _load_table() -> dict[int, tuple[int, int]]:
    """Returns {celsius: (range_min_f, range_max_f)} from asos_lookup_table.csv."""
    table = {}
    with open(_TABLE_CSV) as f:
        for row in csv.DictReader(f):
            table[int(row["celsius"])] = (
                int(row["actual_range_min"]),
                int(row["actual_range_max"]),
            )
    return table

_ASOS = _load_table()


def f_bounds(raw_f: float) -> tuple[int, int]:
    """
    Given a Fahrenheit reading from the stream, return (f_floor, f_ceil):
      f_floor = minimum possible original whole-°F (for HIGH market logic)
      f_ceil  = maximum possible original whole-°F (for LOW  market logic)
    Falls back to computed values if celsius is outside table range.
    """
    celsius_i = round((raw_f - 32) * 5 / 9)
    if celsius_i in _ASOS:
        return _ASOS[celsius_i]
    # Fallback: compute dynamically (same algorithm that built the table)
    base = round(celsius_i * 9 / 5 + 32)
    possible = [f for f in range(base - 3, base + 4)
                if round((f - 32) * 5 / 9) == celsius_i]
    return (min(possible), max(possible)) if possible else (round(raw_f), round(raw_f))


# ── Push stream ───────────────────────────────────────────────────────────────

async def stream_temp(
    token:   str,
    stids:   str | list[str],
    on_temp: Callable[[str, float, int, int, str], Awaitable[None]],
    rewind:  int = 5,
) -> None:
    """
    Connect to the Synoptic push stream and call:
        await on_temp(stid, raw_f, f_floor, f_ceil, obs_time)
    for every new air_temp observation from any station in `stids`.

    Runs forever; reconnects automatically on drop using session ID.
    """
    if isinstance(stids, list):
        stids = ",".join(stids)

    session_id: str | None = None

    while True:
        if session_id:
            url = f"wss://{PUSH_HOST}/feed/{token}/{session_id}"
        else:
            url = (
                f"wss://{PUSH_HOST}/feed/{token}/"
                f"?stid={stids}&vars=air_temp&units=temp%7Cf&rewind={rewind}"
            )

        try:
            async with websockets.connect(url, open_timeout=15) as ws:
                log.info("[weather] Connected  session=%s", session_id or "new")

                async for raw in ws:
                    msg  = json.loads(raw)
                    kind = msg.get("type")

                    if kind == "auth":
                        if msg.get("code") != "success":
                            raise RuntimeError(f"Auth failed: {msg.get('messages')}")
                        session_id = msg["session"]

                    elif kind == "data":
                        for obs in msg.get("data", []):
                            if obs.get("sensor") == "air_temp":
                                raw_f       = float(obs["value"])
                                floor, ceil = f_bounds(raw_f)
                                await on_temp(obs["stid"], raw_f, floor, ceil, obs["date"])

        except websockets.ConnectionClosed as e:
            log.warning("[weather] Connection closed (%s), reconnecting...", e.code)
            await asyncio.sleep(2)

        except Exception as e:
            log.error("[weather] Error: %s  — reconnecting in 5s", e)
            await asyncio.sleep(5)
