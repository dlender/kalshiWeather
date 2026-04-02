"""
Kalshi Weather Market Fetcher
Fetches today's Chicago weather temperature market from Kalshi API.
Also fetches NWS observations for comparison.
Includes backtesting functionality for the arbitrage strategy.
"""

import requests
from datetime import datetime, timezone, timedelta
from typing import Optional, List, Dict
from dataclasses import dataclass
import re


@dataclass
class NWSObservation:
    """Represents a single NWS weather observation."""
    station: str
    timestamp: datetime
    temperature_c: Optional[float]
    temperature_f: Optional[float]
    raw_message: Optional[str]  # Raw METAR string


# =============================================================================
# TEMPERATURE CONVERSION UTILITIES
# =============================================================================
#
# NWS 5-minute stations use this conversion chain:
# 1. Sensor records 1-min averages
# 2. 5 averages combined into 5-min average
# 3. Rounded to nearest whole °F
# 4. Converted to nearest whole °C for transmission
# 5. NWS displays by converting °C back to °F
#
# This introduces ambiguity: multiple original °F values map to same °C.
# The CLI report uses the ORIGINAL sensor value (whole °F), not the converted one.
# =============================================================================

def get_possible_original_f(api_celsius: int) -> List[int]:
    """
    Given a whole degree Celsius from the 5-minute API, return all possible
    original Fahrenheit values that could have produced it.

    Args:
        api_celsius: The whole degree Celsius value from NWS API

    Returns:
        List of possible original whole °F values (usually 1-2 values)
    """
    possible = []
    # Check a range of F values to see which ones round to this C
    for f in range(api_celsius * 9 // 5 + 32 - 3, api_celsius * 9 // 5 + 32 + 4):
        exact_c = (f - 32) * 5 / 9
        if round(exact_c) == api_celsius:
            possible.append(f)
    return sorted(possible)


def get_cli_bounds_from_api(api_celsius: int) -> tuple:
    """
    Given a whole degree Celsius from the 5-minute API, return the min and max
    possible CLI settlement values.

    Args:
        api_celsius: The whole degree Celsius value from NWS API

    Returns:
        Tuple of (min_possible_f, max_possible_f) for CLI settlement
    """
    possible = get_possible_original_f(api_celsius)
    return (min(possible), max(possible))


def get_safe_dead_buckets(api_celsius: int) -> int:
    """
    Given a whole degree Celsius from the 5-minute API, return the highest
    bucket ceiling that is GUARANTEED to be dead for a HIGH market.

    This is conservative - we only count buckets as dead if even the LOWEST
    possible original F value exceeds their ceiling.

    Args:
        api_celsius: The whole degree Celsius value from NWS API

    Returns:
        The highest bucket ceiling guaranteed to be dead
        (buckets with ceiling < this value are definitely dead)
    """
    min_f, max_f = get_cli_bounds_from_api(api_celsius)
    # The CLI high is AT LEAST min_f, so buckets with ceiling < min_f are dead
    return min_f - 1


# Build lookup table for quick reference
CELSIUS_TO_POSSIBLE_F = {}
for c in range(-20, 50):
    CELSIUS_TO_POSSIBLE_F[c] = get_possible_original_f(c)


class NWSClient:
    """Client to fetch weather observations from NWS API."""

    BASE_URL = "https://api.weather.gov"

    # Chicago stations
    CHICAGO_STATIONS = {
        "KORD": "Chicago O'Hare International Airport",
        "KMDW": "Chicago Midway International Airport",
    }

    def __init__(self):
        self.session = requests.Session()
        self.session.headers.update({
            "Accept": "application/geo+json",
            "User-Agent": "KalshiWeatherTracker/1.0 (weather market analysis)"
        })

    def get_latest_observation(self, station_id: str = "KORD") -> Optional[NWSObservation]:
        """
        Get the latest observation from a weather station.

        Args:
            station_id: ICAO station identifier (default: KORD for O'Hare)

        Returns:
            NWSObservation or None if not available
        """
        response = self.session.get(
            f"{self.BASE_URL}/stations/{station_id}/observations/latest"
        )
        response.raise_for_status()

        data = response.json()
        props = data.get("properties", {})

        # Extract temperature (API returns Celsius)
        temp_data = props.get("temperature", {})
        temp_c = temp_data.get("value")
        temp_f = None
        if temp_c is not None:
            temp_f = round(temp_c * 9 / 5 + 32, 1)

        # Parse timestamp
        timestamp_str = props.get("timestamp")
        timestamp = None
        if timestamp_str:
            timestamp = datetime.fromisoformat(timestamp_str.replace("Z", "+00:00"))

        return NWSObservation(
            station=station_id,
            timestamp=timestamp,
            temperature_c=temp_c,
            temperature_f=temp_f,
            raw_message=props.get("rawMessage")
        )

    def get_observations(self, station_id: str = "KORD", limit: int = 24) -> List[NWSObservation]:
        """
        Get recent observations from a weather station.

        Args:
            station_id: ICAO station identifier
            limit: Maximum number of observations to return

        Returns:
            List of NWSObservation objects
        """
        response = self.session.get(
            f"{self.BASE_URL}/stations/{station_id}/observations",
            params={"limit": limit}
        )
        response.raise_for_status()

        data = response.json()
        features = data.get("features", [])

        observations = []
        for feature in features:
            props = feature.get("properties", {})

            temp_data = props.get("temperature", {})
            temp_c = temp_data.get("value")
            temp_f = None
            if temp_c is not None:
                temp_f = round(temp_c * 9 / 5 + 32, 1)

            timestamp_str = props.get("timestamp")
            timestamp = None
            if timestamp_str:
                timestamp = datetime.fromisoformat(timestamp_str.replace("Z", "+00:00"))

            observations.append(NWSObservation(
                station=station_id,
                timestamp=timestamp,
                temperature_c=temp_c,
                temperature_f=temp_f,
                raw_message=props.get("rawMessage")
            ))

        return observations

    def get_todays_high(self, station_id: str = "KORD") -> Optional[dict]:
        """
        Calculate today's high temperature from observations.

        Note: This uses the converted Fahrenheit values from the API,
        which may differ from the CLI settlement value due to rounding.

        Args:
            station_id: ICAO station identifier

        Returns:
            dict with high temp info or None
        """
        observations = self.get_observations(station_id, limit=100)

        if not observations:
            return None

        # Filter to today's observations (in local Chicago time, CST/CDT)
        # Chicago is UTC-6 (CST) or UTC-5 (CDT)
        today_utc = datetime.now(timezone.utc).date()

        todays_obs = [
            obs for obs in observations
            if obs.timestamp and obs.timestamp.date() == today_utc
            and obs.temperature_f is not None
        ]

        if not todays_obs:
            return None

        # Find the highest temperature from API values
        max_obs = max(todays_obs, key=lambda x: x.temperature_f)

        # Also find the highest T-group precise temperature
        tgroup_high_f = None
        tgroup_high_c = None
        tgroup_high_obs = None

        for obs in todays_obs:
            if obs.raw_message:
                precise_c = self.parse_metar_tgroup(obs.raw_message)
                if precise_c is not None:
                    precise_f = round(precise_c * 9 / 5 + 32, 1)
                    if tgroup_high_f is None or precise_f > tgroup_high_f:
                        tgroup_high_f = precise_f
                        tgroup_high_c = precise_c
                        tgroup_high_obs = obs

        return {
            "high_f": max_obs.temperature_f,
            "high_c": max_obs.temperature_c,
            "timestamp": max_obs.timestamp,
            "station": station_id,
            "observation_count": len(todays_obs),
            "raw_metar": max_obs.raw_message,
            # T-group precise high (more reliable for CLI settlement)
            "tgroup_high_f": tgroup_high_f,
            "tgroup_high_c": tgroup_high_c,
            "tgroup_high_timestamp": tgroup_high_obs.timestamp if tgroup_high_obs else None,
            "tgroup_high_metar": tgroup_high_obs.raw_message if tgroup_high_obs else None,
        }

    def parse_metar_tgroup(self, raw_metar: str) -> Optional[float]:
        """
        Parse the T-group from a METAR for precise temperature.

        The T-group format is Txxxxxxxx where:
        - First 4 digits: temperature in tenths of °C (first digit: 0=positive, 1=negative)
        - Last 4 digits: dewpoint in tenths of °C

        Example: T01170100 = 11.7°C temperature, 10.0°C dewpoint

        Args:
            raw_metar: Raw METAR string

        Returns:
            Temperature in Celsius with 0.1° precision, or None if not found
        """
        if not raw_metar:
            return None

        # Look for T-group in remarks
        match = re.search(r'\bT(\d{4})(\d{4})\b', raw_metar)
        if not match:
            return None

        temp_str = match.group(1)
        sign = -1 if temp_str[0] == '1' else 1
        temp_tenths = int(temp_str[1:])
        temp_c = sign * temp_tenths / 10.0

        return temp_c

    def get_observations_for_date(self, station_id: str, target_date: datetime) -> List[NWSObservation]:
        """
        Get all observations for a specific date.

        Args:
            station_id: ICAO station identifier
            target_date: The date to get observations for

        Returns:
            List of NWSObservation objects for that date
        """
        # Fetch a large number of observations to cover multiple days
        all_obs = self.get_observations(station_id, limit=500)

        # Filter to target date
        target_date_only = target_date.date() if isinstance(target_date, datetime) else target_date

        return [
            obs for obs in all_obs
            if obs.timestamp and obs.timestamp.date() == target_date_only
            and obs.temperature_f is not None
        ]

    def get_high_for_date(self, station_id: str, target_date: datetime) -> Optional[dict]:
        """
        Calculate the high temperature for a specific date.

        Args:
            station_id: ICAO station identifier
            target_date: The date to calculate high for

        Returns:
            dict with high temp info or None
        """
        todays_obs = self.get_observations_for_date(station_id, target_date)

        if not todays_obs:
            return None

        # Find the highest temperature from API values
        max_obs = max(todays_obs, key=lambda x: x.temperature_f)

        # Also find the highest T-group precise temperature
        tgroup_high_f = None
        tgroup_high_c = None
        tgroup_high_obs = None

        for obs in todays_obs:
            if obs.raw_message:
                precise_c = self.parse_metar_tgroup(obs.raw_message)
                if precise_c is not None:
                    precise_f = round(precise_c * 9 / 5 + 32, 1)
                    if tgroup_high_f is None or precise_f > tgroup_high_f:
                        tgroup_high_f = precise_f
                        tgroup_high_c = precise_c
                        tgroup_high_obs = obs

        # Calculate estimated CLI settlement (whole degree F)
        cli_estimate = None
        if tgroup_high_f is not None:
            cli_estimate = round(tgroup_high_f)
        elif max_obs.temperature_f is not None:
            cli_estimate = round(max_obs.temperature_f)

        return {
            "date": target_date.date() if isinstance(target_date, datetime) else target_date,
            "high_f": max_obs.temperature_f,
            "high_c": max_obs.temperature_c,
            "timestamp": max_obs.timestamp,
            "station": station_id,
            "observation_count": len(todays_obs),
            "tgroup_high_f": tgroup_high_f,
            "tgroup_high_c": tgroup_high_c,
            "cli_estimate": cli_estimate,
        }


class KalshiWeatherClient:
    """Client to fetch weather markets from Kalshi API."""

    BASE_URL = "https://api.elections.kalshi.com/trade-api/v2"

    def __init__(self):
        self.session = requests.Session()
        self.session.headers.update({
            "Accept": "application/json",
            "Content-Type": "application/json"
        })

    def get_chicago_weather_markets(self, event_ticker: Optional[str] = None) -> dict:
        """
        Fetch Chicago weather temperature markets for today.

        Args:
            event_ticker: Optional event ticker to filter by (e.g., "KXHIGHCHI-26JAN12")

        Returns:
            dict: Market data including prices and contract details
        """
        # Search for Chicago temperature markets
        # Kalshi uses series tickers like "KXHIGHCHI" for Chicago high temp
        params = {
            "status": "open",
            "series_ticker": "KXHIGHCHI",  # Chicago high temperature series
        }

        if event_ticker:
            params["event_ticker"] = event_ticker

        response = self.session.get(
            f"{self.BASE_URL}/markets",
            params=params
        )
        response.raise_for_status()

        return response.json()

    def get_market_by_ticker(self, ticker: str) -> dict:
        """
        Fetch a specific market by its ticker.

        Args:
            ticker: The market ticker (e.g., "KXHIGHCHI-25JAN12")

        Returns:
            dict: Market data
        """
        response = self.session.get(
            f"{self.BASE_URL}/markets/{ticker}"
        )
        response.raise_for_status()

        return response.json()

    def get_events(self, series_ticker: Optional[str] = None) -> dict:
        """
        Fetch events, optionally filtered by series ticker.

        Args:
            series_ticker: Optional series ticker to filter by

        Returns:
            dict: Event data
        """
        params = {}
        if series_ticker:
            params["series_ticker"] = series_ticker

        response = self.session.get(
            f"{self.BASE_URL}/events",
            params=params
        )
        response.raise_for_status()

        return response.json()

    def find_todays_chicago_temp_market(self) -> Optional[dict]:
        """
        Find today's Chicago temperature market.

        Returns:
            dict or None: Today's market data if found
        """
        today = datetime.now(timezone.utc)
        today_str = today.strftime("%y%b%d").upper()  # e.g., "25JAN12"

        # Try common Chicago temperature tickers
        tickers_to_try = [
            f"KXHIGHCHI-{today_str}",  # High temperature
            f"KXLOWCHI-{today_str}",   # Low temperature
        ]

        for ticker in tickers_to_try:
            try:
                market = self.get_market_by_ticker(ticker)
                return market
            except requests.exceptions.HTTPError as e:
                if e.response.status_code == 404:
                    continue
                raise

        # If direct ticker lookup fails, search through markets
        return self._search_chicago_markets(today)

    def _search_chicago_markets(self, target_date: datetime) -> Optional[dict]:
        """Search for Chicago markets matching target date."""
        markets_data = self.get_chicago_weather_markets()
        markets = markets_data.get("markets", [])

        for market in markets:
            # Check if market is for today
            close_time = market.get("close_time", "")
            if close_time and target_date.strftime("%Y-%m-%d") in close_time:
                return {"market": market}

        return None

    def get_settled_markets(self, series_ticker: str = "KXHIGHCHI", limit: int = 200) -> List[dict]:
        """
        Fetch settled/closed markets for backtesting.

        Args:
            series_ticker: Series ticker to filter by
            limit: Maximum number of markets to return

        Returns:
            List of settled market data
        """
        all_markets = []
        cursor = None

        while len(all_markets) < limit:
            params = {
                "series_ticker": series_ticker,
                "status": "settled",
                "limit": min(100, limit - len(all_markets)),
            }
            if cursor:
                params["cursor"] = cursor

            response = self.session.get(
                f"{self.BASE_URL}/markets",
                params=params
            )
            response.raise_for_status()
            data = response.json()

            markets = data.get("markets", [])
            if not markets:
                break

            all_markets.extend(markets)
            cursor = data.get("cursor")
            if not cursor:
                break

        return all_markets

    def get_markets_for_event(self, event_ticker: str) -> List[dict]:
        """
        Fetch all markets (all buckets) for a specific event.

        Args:
            event_ticker: Event ticker (e.g., "KXHIGHCHI-26JAN10")

        Returns:
            List of market data for all buckets in that event
        """
        params = {
            "event_ticker": event_ticker,
        }

        response = self.session.get(
            f"{self.BASE_URL}/markets",
            params=params
        )
        response.raise_for_status()

        return response.json().get("markets", [])

    def get_event_by_ticker(self, event_ticker: str) -> Optional[dict]:
        """
        Fetch event details by ticker.

        Args:
            event_ticker: Event ticker

        Returns:
            Event data or None
        """
        try:
            response = self.session.get(
                f"{self.BASE_URL}/events/{event_ticker}"
            )
            response.raise_for_status()
            return response.json().get("event")
        except requests.exceptions.HTTPError:
            return None


def format_market_info(market_data: dict) -> str:
    """Format market data for display."""
    if not market_data:
        return "No Chicago weather market found for today."

    market = market_data.get("market", {})

    output = []
    output.append("=" * 60)
    output.append("CHICAGO WEATHER TEMPERATURE MARKET")
    output.append("=" * 60)
    output.append(f"Ticker: {market.get('ticker', 'N/A')}")
    output.append(f"Title: {market.get('title', 'N/A')}")
    output.append(f"Subtitle: {market.get('subtitle', 'N/A')}")
    output.append(f"Status: {market.get('status', 'N/A')}")
    output.append("-" * 60)

    # Price information (prices are in cents)
    yes_bid = market.get("yes_bid", 0)
    yes_ask = market.get("yes_ask", 0)
    last_price = market.get("last_price", 0)

    output.append(f"Last Price: {last_price}¢ ({last_price}% implied probability)")
    output.append(f"Yes Bid: {yes_bid}¢ | Yes Ask: {yes_ask}¢")

    no_bid = market.get("no_bid", 0)
    no_ask = market.get("no_ask", 0)
    output.append(f"No Bid: {no_bid}¢ | No Ask: {no_ask}¢")

    output.append("-" * 60)
    output.append(f"Volume: {market.get('volume', 0)} contracts")
    output.append(f"Open Interest: {market.get('open_interest', 0)}")
    output.append(f"Close Time: {market.get('close_time', 'N/A')}")
    output.append("=" * 60)

    return "\n".join(output)


def format_nws_observation(obs: NWSObservation, nws_client: NWSClient) -> str:
    """Format a single NWS observation for display."""
    output = []

    # Parse T-group for precise temperature if available
    precise_temp_c = nws_client.parse_metar_tgroup(obs.raw_message)
    precise_temp_f = None
    if precise_temp_c is not None:
        precise_temp_f = round(precise_temp_c * 9 / 5 + 32, 1)

    time_str = obs.timestamp.strftime("%Y-%m-%d %H:%M UTC") if obs.timestamp else "N/A"

    output.append(f"  Time: {time_str}")
    output.append(f"  API Temperature: {obs.temperature_f}°F ({obs.temperature_c}°C)")

    if precise_temp_f is not None:
        output.append(f"  T-Group Temperature: {precise_temp_f}°F ({precise_temp_c}°C) [MORE PRECISE]")
        # Show the difference
        if obs.temperature_f is not None:
            diff = abs(precise_temp_f - obs.temperature_f)
            if diff > 0.1:
                output.append(f"  Rounding Difference: {diff:.1f}°F")

    if obs.raw_message:
        # Truncate long METAR strings
        metar = obs.raw_message[:80] + "..." if len(obs.raw_message) > 80 else obs.raw_message
        output.append(f"  Raw METAR: {metar}")

    return "\n".join(output)


def format_nws_high(high_data: dict, nws_client: NWSClient) -> str:
    """Format today's high temperature data from NWS."""
    output = []
    output.append("=" * 60)
    output.append("NWS OBSERVED HIGH TEMPERATURE")
    output.append("=" * 60)
    output.append(f"Station: {high_data['station']} ({NWSClient.CHICAGO_STATIONS.get(high_data['station'], 'Unknown')})")
    output.append(f"Observations Today: {high_data['observation_count']}")
    output.append("-" * 60)

    # API-reported high (may have rounding artifacts)
    output.append(f"API High: {high_data['high_f']}°F ({high_data['high_c']}°C)")
    if high_data.get('timestamp'):
        output.append(f"  Recorded At: {high_data['timestamp'].strftime('%Y-%m-%d %H:%M UTC')}")

    output.append("-" * 60)

    # T-group precise high (more reliable for CLI settlement)
    tgroup_high_f = high_data.get('tgroup_high_f')
    tgroup_high_c = high_data.get('tgroup_high_c')

    if tgroup_high_f is not None:
        output.append("T-GROUP PRECISE HIGH (Best estimate for CLI):")
        output.append(f"  Temperature: {tgroup_high_f}°F ({tgroup_high_c}°C)")

        # Calculate what the CLI will report (whole degree F)
        cli_estimate = round(tgroup_high_f)
        output.append(f"  >>> ESTIMATED CLI SETTLEMENT: {cli_estimate}°F <<<")

        if high_data.get('tgroup_high_timestamp'):
            output.append(f"  Recorded At: {high_data['tgroup_high_timestamp'].strftime('%Y-%m-%d %H:%M UTC')}")

        # Show which Kalshi bucket this falls into
        output.append("")
        output.append(f"  Kalshi Bucket: {cli_estimate - 1}-{cli_estimate}°F should WIN")
    else:
        output.append("T-GROUP: Not available in recent METARs")
        output.append("  (Hourly METARs at :51 past hour typically have T-groups)")

    output.append("-" * 60)
    output.append("NOTE: CLI (settlement value) uses original sensor data.")
    output.append("T-group temps are more accurate than API temps for prediction.")
    output.append("=" * 60)

    return "\n".join(output)


def parse_bucket_range(title: str) -> Optional[tuple]:
    """
    Parse temperature range from market title.

    Args:
        title: Market title like "Will the high temp in Chicago be 38-39° on Jan 11, 2026?"
               or "Will the high temp in Chicago be >39° on Jan 11, 2026?"
               or "Will the high temp in Chicago be <32° on Jan 11, 2026?"

    Returns:
        Tuple of (low_bound, high_bound) or None
    """
    # Match "<X°" (less than X)
    match = re.search(r'be\s*<\s*(\d+)°', title)
    if match:
        high = int(match.group(1)) - 1  # <32 means 31 or lower
        return (None, high)

    # Match ">X°" (greater than X)
    match = re.search(r'be\s*>\s*(\d+)°', title)
    if match:
        low = int(match.group(1)) + 1  # >39 means 40 or higher
        return (low, None)

    # Match "X-Y°" range (e.g., "38-39°")
    match = re.search(r'be\s+(\d+)-(\d+)°', title)
    if match:
        low = int(match.group(1))
        high = int(match.group(2))
        return (low, high)

    # Legacy format: "X° F or lower"
    match = re.search(r'(\d+)°\s*F or lower', title)
    if match:
        high = int(match.group(1))
        return (None, high)

    # Legacy format: "X° F or higher"
    match = re.search(r'(\d+)°\s*F or higher', title)
    if match:
        low = int(match.group(1))
        return (low, None)

    # Legacy format: "between X° F and Y° F"
    match = re.search(r'between\s+(\d+)°\s*F\s+and\s+(\d+)°\s*F', title)
    if match:
        low = int(match.group(1))
        high = int(match.group(2))
        return (low, high)

    return None


def bucket_contains_temp(bucket_range: tuple, temp: int) -> bool:
    """Check if a temperature falls within a bucket range."""
    low, high = bucket_range
    if low is None:
        return temp <= high
    if high is None:
        return temp >= low
    return low <= temp <= high


def bucket_is_dead_for_high(bucket_range: tuple, current_high: int) -> bool:
    """
    Check if a bucket is mathematically eliminated for a HIGH temp market.

    For HIGH markets, once we observe a temperature, the daily high can only
    go UP from there. So any bucket with a ceiling BELOW our current observation
    is dead.

    Args:
        bucket_range: (low, high) tuple for the bucket
        current_high: Current observed high temperature

    Returns:
        True if this bucket cannot possibly win
    """
    low, high = bucket_range
    if high is None:
        # "X or higher" bucket - never dead once we're at or above X
        return False
    # If our current high already exceeds this bucket's ceiling, it's dead
    return current_high > high


@dataclass
class BacktestTrade:
    """Represents a simulated trade in the backtest."""
    date: datetime
    event_ticker: str
    market_ticker: str
    bucket_title: str
    bucket_range: tuple
    action: str  # "BUY_NO"
    entry_price: float  # Price paid for NO (in cents)
    observed_high: int  # T-group high when trade was made
    final_settlement: int  # Actual CLI settlement
    bucket_won: bool  # Did this bucket win?
    profit: float  # Profit in cents (100 - entry if we win, -entry if we lose)


def run_backtest(days: int = 7) -> Dict:
    """
    Run a backtest of the arbitrage strategy over the last N days.

    Strategy: Use the 5-minute API conversion logic to determine which buckets
    are SAFELY dead (accounting for C->F conversion ambiguity).

    For each day:
    1. Get the winning bucket from Kalshi settlement
    2. Determine the CLI settlement temp
    3. Figure out what the max API Celsius reading would have been
    4. Calculate which buckets were SAFELY dead (using conservative conversion)
    5. Check if those buckets had any YES value we could have captured

    Args:
        days: Number of days to backtest

    Returns:
        Dict with backtest results
    """
    print("=" * 70)
    print("BACKTEST: Dead Bucket Arbitrage (with Conversion Logic)")
    print("=" * 70)
    print(f"Testing last {days} days...")
    print()
    print("Strategy: Only short buckets that are SAFELY dead after accounting")
    print("for the C->F conversion ambiguity (+/- 1F uncertainty)")
    print()

    kalshi_client = KalshiWeatherClient()

    # Get dates to test (excluding today since it hasn't settled)
    today = datetime.now(timezone.utc).date()
    test_dates = [(today - timedelta(days=i)) for i in range(1, days + 1)]

    results = {
        "trades": [],
        "days_analyzed": 0,
        "days_with_data": 0,
        "total_profit": 0,
        "winning_trades": 0,
        "losing_trades": 0,
        "daily_summaries": [],
    }

    print("Fetching Kalshi settled markets...")
    all_settled = kalshi_client.get_settled_markets("KXHIGHCHI", limit=200)
    print(f"Fetched {len(all_settled)} settled markets")
    print()

    # Group markets by event (date)
    events_markets = {}
    for market in all_settled:
        ticker = market.get("ticker", "")
        parts = ticker.split("-")
        if len(parts) >= 2:
            event_ticker = f"{parts[0]}-{parts[1]}"
            if event_ticker not in events_markets:
                events_markets[event_ticker] = []
            events_markets[event_ticker].append(market)

    for test_date in test_dates:
        results["days_analyzed"] += 1
        date_str = test_date.strftime("%Y-%m-%d")
        event_ticker = f"KXHIGHCHI-{test_date.strftime('%y%b%d').upper()}"

        print(f"--- {date_str} ({event_ticker}) ---")

        markets = events_markets.get(event_ticker, [])
        if not markets:
            print(f"  No Kalshi markets found")
            print()
            continue

        results["days_with_data"] += 1

        # Find the winning bucket to determine actual CLI settlement
        winning_bucket = None
        cli_settlement = None
        all_buckets = []

        for market in markets:
            title = market.get("title", "")
            ticker = market.get("ticker", "")
            result = market.get("result", "")
            yes_bid = market.get("yes_bid", 0)
            last_price = market.get("last_price", 0)

            bucket_range = parse_bucket_range(title)
            if bucket_range is None:
                continue

            bucket_won = result == "yes"

            all_buckets.append({
                "ticker": ticker,
                "title": title,
                "range": bucket_range,
                "result": result,
                "bucket_won": bucket_won,
                "yes_bid": yes_bid,
                "last_price": last_price,
            })

            if bucket_won:
                winning_bucket = all_buckets[-1]
                # Get CLI settlement from winning bucket
                low, high = bucket_range
                if low is not None and high is not None:
                    # For a range like 41-42, CLI could be 41 or 42
                    # We'll use the high end for "safe dead" calculation
                    cli_settlement = high
                elif low is not None:
                    # ">X" bucket - CLI is at least X+1
                    cli_settlement = low + 1
                elif high is not None:
                    # "<X" bucket - CLI is at most X-1
                    cli_settlement = high

        if not winning_bucket:
            print(f"  No winning bucket found")
            print()
            continue

        # Calculate what the 5-minute API would have shown
        # CLI settlement F -> what C would round to it?
        # If CLI=42, possible C values are those where round((F-32)*5/9) produces
        # a C that maps back to 42. From our table: 6C -> [42,43]
        # So if we saw 6C, we'd know CLI is 42 or 43

        # For safe trading: find which C value would have been seen
        # and what's the MINIMUM F that C implies
        api_celsius = round((cli_settlement - 32) * 5 / 9)
        possible_f = get_possible_original_f(api_celsius)
        min_possible_f = min(possible_f)
        safe_dead_ceiling = min_possible_f - 1

        print(f"  CLI Settlement: {cli_settlement}F (bucket: {winning_bucket['title']})")
        print(f"  API would show: {api_celsius}C -> possible F: {possible_f}")
        print(f"  SAFE dead ceiling: {safe_dead_ceiling}F (buckets <= this are guaranteed dead)")

        # Find all SAFELY dead buckets
        safe_dead_buckets = []
        for bucket in all_buckets:
            low, high = bucket["range"]

            # Skip the winning bucket
            if bucket["bucket_won"]:
                continue

            # A bucket is SAFELY dead if its ceiling is below our safe threshold
            if high is not None and high <= safe_dead_ceiling:
                safe_dead_buckets.append(bucket)

        print(f"  Safe dead buckets: {len(safe_dead_buckets)}")

        day_profit = 0
        day_trades = 0

        for bucket in safe_dead_buckets:
            yes_price = bucket["last_price"] or bucket["yes_bid"] or 0

            if yes_price > 0:
                no_price = 100 - yes_price
                profit = 100 - no_price  # = yes_price

                results["winning_trades"] += 1
                day_profit += profit
                day_trades += 1

                trade = BacktestTrade(
                    date=test_date,
                    event_ticker=event_ticker,
                    market_ticker=bucket["ticker"],
                    bucket_title=bucket["title"],
                    bucket_range=bucket["range"],
                    action="BUY_NO",
                    entry_price=no_price,
                    observed_high=cli_settlement,
                    final_settlement=cli_settlement,
                    bucket_won=False,
                    profit=profit,
                )
                results["trades"].append(trade)

                print(f"    SAFE Trade: NO @ {no_price}c on '{bucket['title']}' -> WIN +{profit}c")

        results["total_profit"] += day_profit
        results["daily_summaries"].append({
            "date": test_date,
            "cli_settlement": cli_settlement,
            "api_celsius": api_celsius,
            "safe_dead_ceiling": safe_dead_ceiling,
            "trades": day_trades,
            "profit": day_profit,
        })

        if day_trades > 0:
            print(f"  Day P&L: +{day_profit}c from {day_trades} trade(s)")
        else:
            print(f"  No safely tradeable dead buckets")
        print()

    # Print summary
    print("=" * 70)
    print("BACKTEST SUMMARY")
    print("=" * 70)
    print(f"Days analyzed: {results['days_analyzed']}")
    print(f"Days with market data: {results['days_with_data']}")
    print(f"Total SAFE trades: {len(results['trades'])}")
    print(f"Winning trades: {results['winning_trades']}")
    print(f"Losing trades: {results['losing_trades']}")
    if results['trades']:
        win_rate = results['winning_trades'] / len(results['trades']) * 100
        print(f"Win rate: {win_rate:.1f}%")
    print(f"Total P&L: +{results['total_profit']}c (${results['total_profit']/100:.2f})")
    if results['trades']:
        avg_profit = results['total_profit'] / len(results['trades'])
        print(f"Avg profit per trade: {avg_profit:.1f}c")
    print("=" * 70)
    print()
    print("CONVERSION TABLE USED:")
    print("  API Celsius -> Possible CLI Fahrenheit -> Safe Dead Ceiling")
    for c in range(0, 12):
        poss = CELSIUS_TO_POSSIBLE_F.get(c, [])
        safe = min(poss) - 1 if poss else "?"
        print(f"  {c}C -> {poss} -> buckets <= {safe}F are SAFE to short")
    print()
    print("NOTE: This backtest uses settlement prices. Real trading requires")
    print("monitoring the 5-minute API and acting when readings come in.")

    return results


def main():
    """Main function to fetch and display Chicago weather market."""
    print("=" * 60)
    print("CHICAGO WEATHER MARKET TRACKER")
    print("=" * 60)
    print()

    kalshi_client = KalshiWeatherClient()
    nws_client = NWSClient()

    # Get today's date
    today = datetime.now(timezone.utc)
    today_event = f"KXHIGHCHI-{today.strftime('%y%b%d').upper()}"

    print(f"Date: {today.strftime('%B %d, %Y')}")
    print(f"Event Ticker: {today_event}")
    print()

    # =====================================================
    # SECTION 1: NWS Observations
    # =====================================================
    print("FETCHING NWS OBSERVATIONS...")
    print()

    try:
        # Get current observation
        for station in ["KORD", "KMDW"]:
            try:
                latest = nws_client.get_latest_observation(station)
                if latest:
                    print(f"Latest {station} Observation:")
                    print(format_nws_observation(latest, nws_client))
                    print()
            except requests.exceptions.RequestException as e:
                print(f"Error fetching {station}: {e}")

        # Get today's high
        print("-" * 60)
        high_data = nws_client.get_todays_high("KORD")
        if high_data:
            print(format_nws_high(high_data, nws_client))
        else:
            print("Could not determine today's high temperature from NWS.")
        print()

    except requests.exceptions.RequestException as e:
        print(f"Error fetching NWS data: {e}")
        print()

    # =====================================================
    # SECTION 2: Kalshi Markets
    # =====================================================
    print("FETCHING KALSHI MARKETS...")
    print()

    try:
        todays_markets = kalshi_client.get_chicago_weather_markets(event_ticker=today_event)
        markets = todays_markets.get("markets", [])

        if markets:
            print(f"Found {len(markets)} Chicago weather market(s) for today:")
            print()
            for market in markets:
                print(format_market_info({"market": market}))
                print()
        else:
            print(f"No markets found for today ({today_event}).")
            print("Fetching all available Chicago weather markets...")
            print()

            all_markets = kalshi_client.get_chicago_weather_markets()
            markets = all_markets.get("markets", [])

            if markets:
                print(f"Found {len(markets)} Chicago weather market(s):")
                print()
                for market in markets[:10]:
                    print(format_market_info({"market": market}))
                    print()
            else:
                print("No Chicago weather markets currently available.")

    except requests.exceptions.RequestException as e:
        print(f"Error fetching Kalshi data: {e}")
        return 1

    return 0


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1 and sys.argv[1] == "backtest":
        days = int(sys.argv[2]) if len(sys.argv) > 2 else 7
        run_backtest(days)
    else:
        exit(main())
