import unittest
from datetime import datetime, timezone
from unittest.mock import patch
from zoneinfo import ZoneInfo

import trader


class FakeKalshiClient:
    def __init__(self, markets_by_event):
        self.markets_by_event = markets_by_event
        self.calls = []
        self.orders = []

    async def get_markets(self, event_ticker: str):
        self.calls.append(event_ticker)
        return self.markets_by_event.get(event_ticker, [])

    async def place_no_order(self, ticker: str, count: int, no_price: int):
        self.orders.append((ticker, count, no_price))
        return {"order_id": "test-order", "status": "accepted"}


class FixedDateTime(datetime):
    current = datetime(2026, 4, 2, 9, 0, tzinfo=timezone.utc)

    @classmethod
    def now(cls, tz=None):
        if tz is None:
            return cls.current.replace(tzinfo=None)
        return cls.current.astimezone(tz)


class ResolveEventTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.load_traded_patcher = patch("trader._load_traded", return_value=set())
        self.load_traded_patcher.start()

    async def asyncTearDown(self):
        self.load_traded_patcher.stop()

    async def test_prewarms_tomorrow_after_1150pm_local(self):
        client = FakeKalshiClient(
            {
                "KXLOWTLAX-26APR02": [
                    {
                        "ticker": "KXLOWTLAX-26APR02-B60.5",
                        "status": "open",
                        "close_time": "2026-04-03T07:59:00Z",
                    }
                ],
                "KXLOWTLAX-26APR03": [
                    {
                        "ticker": "KXLOWTLAX-26APR03-B60.5",
                        "status": "open",
                        "close_time": "2026-04-04T07:59:00Z",
                    }
                ],
            }
        )
        state = trader.TraderState(client)
        FixedDateTime.current = datetime(2026, 4, 2, 23, 55, tzinfo=ZoneInfo("America/Los_Angeles")).astimezone(timezone.utc)

        with patch("trader.datetime", FixedDateTime):
            event = await state._resolve_event("KXLOWTLAX", ZoneInfo("America/Los_Angeles"))

        self.assertEqual(event, "KXLOWTLAX-26APR02")
        self.assertEqual(client.calls, ["KXLOWTLAX-26APR03", "KXLOWTLAX-26APR02"])

    async def test_switches_immediately_after_midnight_even_with_fresh_cache(self):
        client = FakeKalshiClient(
            {
                "KXLOWTLAX-26APR02": [
                    {
                        "ticker": "KXLOWTLAX-26APR02-B60.5",
                        "status": "open",
                        "close_time": "2026-04-03T07:59:00Z",
                    }
                ],
                "KXLOWTLAX-26APR03": [
                    {
                        "ticker": "KXLOWTLAX-26APR03-B60.5",
                        "status": "open",
                        "close_time": "2026-04-04T07:59:00Z",
                    }
                ],
            }
        )
        state = trader.TraderState(client)

        monotonic_values = iter([1000.0, 1000.0, 1000.0, 1001.0, 1001.0, 1001.0])
        with patch("trader.time.monotonic", side_effect=lambda: next(monotonic_values, 1001.0)):
            FixedDateTime.current = datetime(2026, 4, 2, 23, 59, tzinfo=ZoneInfo("America/Los_Angeles")).astimezone(timezone.utc)
            with patch("trader.datetime", FixedDateTime):
                first = await state._resolve_event("KXLOWTLAX", ZoneInfo("America/Los_Angeles"))

            FixedDateTime.current = datetime(2026, 4, 3, 0, 1, tzinfo=ZoneInfo("America/Los_Angeles")).astimezone(timezone.utc)
            with patch("trader.datetime", FixedDateTime):
                second = await state._resolve_event("KXLOWTLAX", ZoneInfo("America/Los_Angeles"))

        self.assertEqual(first, "KXLOWTLAX-26APR02")
        self.assertEqual(second, "KXLOWTLAX-26APR03")

    async def test_keeps_today_market_before_city_midnight(self):
        client = FakeKalshiClient(
            {
                "KXLOWTLAX-26APR02": [
                    {
                        "ticker": "KXLOWTLAX-26APR02-B60.5",
                        "status": "open",
                        "close_time": "2026-04-03T07:59:00Z",
                    }
                ],
                "KXLOWTLAX-26APR03": [
                    {
                        "ticker": "KXLOWTLAX-26APR03-B60.5",
                        "status": "open",
                        "close_time": "2026-04-04T07:59:00Z",
                    }
                ],
            }
        )
        state = trader.TraderState(client)
        FixedDateTime.current = datetime(2026, 4, 2, 9, 0, tzinfo=timezone.utc)

        with patch("trader.datetime", FixedDateTime):
            event = await state._resolve_event("KXLOWTLAX", ZoneInfo("America/Los_Angeles"))

        self.assertEqual(event, "KXLOWTLAX-26APR02")
        self.assertEqual(client.calls, ["KXLOWTLAX-26APR02"])

    async def test_switches_to_next_day_market_at_city_midnight(self):
        client = FakeKalshiClient(
            {
                "KXLOWTLAX-26APR02": [
                    {
                        "ticker": "KXLOWTLAX-26APR02-B60.5",
                        "status": "open",
                        "close_time": "2026-04-03T07:59:00Z",
                    }
                ],
                "KXLOWTLAX-26APR03": [
                    {
                        "ticker": "KXLOWTLAX-26APR03-B60.5",
                        "status": "open",
                        "close_time": "2026-04-04T07:59:00Z",
                    }
                ],
            }
        )
        state = trader.TraderState(client)
        FixedDateTime.current = datetime(2026, 4, 3, 7, 1, tzinfo=timezone.utc)

        with patch("trader.datetime", FixedDateTime):
            event = await state._resolve_event("KXLOWTLAX", ZoneInfo("America/Los_Angeles"))

        self.assertEqual(event, "KXLOWTLAX-26APR03")
        self.assertEqual(client.calls, ["KXLOWTLAX-26APR03"])

    async def test_on_temp_trades_using_next_day_buckets_after_midnight(self):
        next_day_markets = [
            {
                "ticker": "KXLOWTLAX-26APR03-B60.5",
                "status": "open",
                "close_time": "2026-04-04T07:59:00Z",
                "yes_bid": 12,
                "yes_ask": 15,
                "floor_strike": 61,
                "cap_strike": 62,
            }
        ]
        client = FakeKalshiClient(
            {
                "KXLOWTLAX-26APR02": [
                    {
                        "ticker": "KXLOWTLAX-26APR02-B60.5",
                        "status": "open",
                        "close_time": "2026-04-03T07:59:00Z",
                    }
                ],
                "KXLOWTLAX-26APR03": next_day_markets,
            }
        )
        state = trader.TraderState(client)
        FixedDateTime.current = datetime(2026, 4, 3, 7, 1, tzinfo=timezone.utc)
        seen = {}

        def fake_no_targets(f_floor, f_ceil, markets, side, min_yes, traded_set):
            if not markets:
                return []
            seen["markets"] = markets
            seen["side"] = side
            return [
                {
                    "ticker": markets[0]["ticker"],
                    "no_price": 88,
                    "yes_price": 15,
                    "floor_strike": markets[0]["floor_strike"],
                    "cap_strike": markets[0]["cap_strike"],
                }
            ]

        with patch("trader.datetime", FixedDateTime), patch("trader.no_targets", side_effect=fake_no_targets):
            await state.on_temp("KLAX1M", 58.0, 57, 59, "2026-04-03T07:01:00Z")

        self.assertEqual(seen["side"], "low")
        self.assertEqual(seen["markets"], next_day_markets)
        self.assertIn("KXLOWTLAX-26APR03", client.calls)
        self.assertEqual(client.orders, [("KXLOWTLAX-26APR03-B60.5", trader.ORDER_SIZE, 88)])


if __name__ == "__main__":
    unittest.main()
