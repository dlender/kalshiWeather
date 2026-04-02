"""
logic.py — Determine which NO positions to buy given a live temperature reading.

Two conservative bounds (from asos_lookup_table.csv) are used:
  f_floor = actual_range_min  —  day's max is GUARANTEED ≥ f_floor
  f_ceil  = actual_range_max  —  day's min is GUARANTEED ≤ f_ceil

Rules
─────
LOW  market (KXLOWTCHI): day's min ≤ f_ceil  →  buy NO where floor_strike > f_ceil
HIGH market (KXHIGHCHI): day's max ≥ f_floor →  buy NO where cap_strike   < f_floor

cap_strike is inclusive (B40.5 covers 40 AND 41, cap=41).
floor_strike is inclusive (B37.5 covers 37 AND 38, floor=37).

Price filter: skip if YES ask < min_yes (market already priced it in).
"""

from __future__ import annotations


def no_targets(
    f_floor:    int,            # actual_range_min — for HIGH market
    f_ceil:     int,            # actual_range_max — for LOW market
    markets:    list[dict],
    side:       str,            # "low" or "high"
    min_yes:    int = 5,
    traded_set: set[str] | None = None,
) -> list[dict]:
    """
    Return markets where buying NO is definitively warranted.

    Each returned dict is the original market dict plus:
      no_price  : int — cents to pay
      yes_price : int — YES ask at decision time (for logging)
    """
    targets    = []
    traded_set = traded_set or set()

    for m in markets:
        ticker = m.get("ticker", "")

        if ticker in traded_set:
            continue

        if m.get("status") not in ("open", "active"):
            continue

        floor = m.get("floor_strike")
        cap   = m.get("cap_strike")

        # ── Is this bracket impossible? ───────────────────────────────────────
        # Bounded brackets (B40.5): both floor and cap are set, both inclusive.
        # Terminal brackets (T65, T72): one side is None, and the named strike
        #   is EXCLUSIVE — T65 covers high ≤ 64 (cap=65), T72 covers high ≥ 73 (floor=72).
        impossible = False
        terminal = (floor is None) or (cap is None)

        if side == "low":
            # day's min ≤ f_ceil  →  buy NO where bracket is entirely above f_ceil
            # Bounded: floor is inclusive → impossible if floor > f_ceil
            # Terminal-high (cap=None): floor is exclusive → covers ≥ floor+1
            #   → impossible if floor+1 > f_ceil  i.e. floor >= f_ceil
            if floor is not None:
                threshold = f_ceil if not terminal else f_ceil - 1
                if floor > threshold:
                    impossible = True

        elif side == "high":
            # day's max ≥ f_floor  →  buy NO where bracket is entirely below f_floor
            # Bounded: cap is inclusive → impossible if cap < f_floor
            # Terminal-low (floor=None): cap is exclusive → covers ≤ cap-1
            #   → impossible if cap-1 < f_floor  i.e. cap <= f_floor
            if cap is not None:
                threshold = f_floor if not terminal else f_floor + 1
                if cap < threshold:
                    impossible = True

        if not impossible:
            continue

        # ── Price filter ──────────────────────────────────────────────────────
        def _cents(key_int, key_dollars):
            v = m.get(key_int)
            if v is not None:
                return int(v)
            d = m.get(key_dollars)
            if d is not None:
                return round(float(d) * 100)
            return None

        yes_bid = _cents("yes_bid", "yes_bid_dollars")
        yes_ask = _cents("yes_ask", "yes_ask_dollars")

        if yes_bid is None or yes_ask is None:
            continue

        if yes_ask < min_yes:
            continue

        no_price = 100 - yes_bid

        result = dict(m)
        result["no_price"]  = no_price
        result["yes_price"] = yes_ask
        targets.append(result)

    return targets
