"""
kalshi.py — Kalshi REST API client with RSA-PSS request signing.

Signing format (from C++ reference implementation):
  message = timestamp_ms + METHOD + "/trade-api/v2" + path_with_querystring
  signature = base64( RSA-PSS-SHA256(message) )

Headers:
  KALSHI-ACCESS-KEY       : API key ID
  KALSHI-ACCESS-SIGNATURE : base64 signature
  KALSHI-ACCESS-TIMESTAMP : milliseconds since epoch (string)

Credentials loaded from .env:
  KALSHI_API_KEY_ID
  KALSHI_PRIVATE_KEY  (path to .pem file)
"""

import base64
import os
import time
from pathlib import Path
from urllib.parse import urlencode

import aiohttp
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding as asym_padding
from dotenv import load_dotenv

_here = Path(__file__).parent
load_dotenv(_here / ".env")

API_PREFIX = "/trade-api/v2"
BASE_URL   = "https://api.elections.kalshi.com" + API_PREFIX


class KalshiClient:
    def __init__(self):
        key_id   = os.environ.get("KALSHI_API_KEY_ID", "").strip()
        pem_path = os.environ.get("KALSHI_PRIVATE_KEY", "").strip()

        if not key_id:
            raise RuntimeError("KALSHI_API_KEY_ID not set in .env")
        if not pem_path:
            raise RuntimeError("KALSHI_PRIVATE_KEY not set in .env")

        pem_file = Path(pem_path)
        if not pem_file.exists():
            pem_file = _here / pem_path
        if not pem_file.exists():
            raise RuntimeError(f"Private key file not found: {pem_path}")

        self.key_id       = key_id
        self._private_key = serialization.load_pem_private_key(
            pem_file.read_bytes(), password=None
        )
        self._session: aiohttp.ClientSession | None = None

    def _get_session(self) -> aiohttp.ClientSession:
        if self._session is None or self._session.closed:
            connector = aiohttp.TCPConnector(
                limit=10,
                keepalive_timeout=60,
                enable_cleanup_closed=True,
            )
            self._session = aiohttp.ClientSession(
                connector=connector,
                headers={"Content-Type": "application/json"},
            )
        return self._session

    async def close(self) -> None:
        if self._session and not self._session.closed:
            await self._session.close()

    # ── Signing ───────────────────────────────────────────────────────────────

    def _sign(self, method: str, path_with_qs: str) -> dict:
        ts  = str(int(time.time() * 1000))
        msg = ts + method.upper() + API_PREFIX + path_with_qs
        sig = self._private_key.sign(
            msg.encode(),
            asym_padding.PSS(
                mgf=asym_padding.MGF1(hashes.SHA256()),
                salt_length=asym_padding.PSS.DIGEST_LENGTH,
            ),
            hashes.SHA256(),
        )
        return {
            "KALSHI-ACCESS-KEY":       self.key_id,
            "KALSHI-ACCESS-TIMESTAMP": ts,
            "KALSHI-ACCESS-SIGNATURE": base64.b64encode(sig).decode(),
        }

    # ── HTTP ──────────────────────────────────────────────────────────────────

    async def _get(self, path: str, params: dict | None = None) -> dict:
        qs           = urlencode(params) if params else ""
        path_with_qs = f"{path}?{qs}" if qs else path
        headers      = self._sign("GET", path_with_qs)
        session      = self._get_session()
        async with session.get(BASE_URL + path_with_qs, headers=headers) as resp:
            resp.raise_for_status()
            return await resp.json()

    async def _post(self, path: str, body: dict) -> dict:
        headers = self._sign("POST", path)
        session = self._get_session()
        async with session.post(BASE_URL + path, json=body, headers=headers) as resp:
            if not resp.ok:
                text = await resp.text()
                raise aiohttp.ClientResponseError(
                    resp.request_info, resp.history,
                    status=resp.status, message=text,
                )
            return await resp.json()

    # ── Markets ───────────────────────────────────────────────────────────────

    async def get_markets(self, event_ticker: str) -> list[dict]:
        """Return all markets for an event ticker, handling pagination."""
        markets = []
        cursor  = None
        while True:
            params = {"event_ticker": event_ticker, "limit": 100}
            if cursor:
                params["cursor"] = cursor
            body   = await self._get("/markets", params)
            chunk  = body.get("markets", [])
            markets.extend(chunk)
            cursor = body.get("cursor")
            if not cursor or not chunk:
                break
        return markets

    # ── Orders ────────────────────────────────────────────────────────────────

    async def place_no_order(self, ticker: str, count: int, no_price: int) -> dict:
        """Buy NO contracts at a limit price. no_price in cents (1-99)."""
        body = {
            "ticker":          ticker,
            "action":          "buy",
            "type":            "limit",
            "side":            "no",
            "count":           count,
            "no_price":        no_price,
            "client_order_id": f"weather_{int(time.time() * 1000)}",
        }
        resp = await self._post("/portfolio/orders", body)
        return resp.get("order", resp)
