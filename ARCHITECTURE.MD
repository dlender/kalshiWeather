# MADIS → Kalshi Trading System Architecture

## Overview

This system ingests real-time observational weather data (primarily MADIS METAR),
normalizes it, and reacts directly in Kalshi markets using a fully event-driven,
low-latency architecture.

Key characteristics:
- **No forecasting**: strategy reacts only to incoming observations
- **Multiple data sources** (MADIS + others)
- **Multiple stations** (e.g., KMDW, KMIA)
- **Multiple markets per city**
- **Runtime market discovery**: automatically discover available Kalshi weather markets
- **Dead bucket detection**: skip markets with no liquidity via WebSocket orderbook
- **Single trading engine** with centralized risk
- **NATS event bus** for clean fan-in and extensibility

---

## High-Level Topology

```
             ┌─────────────────┐
             │   nats-server   │
             └────────┬────────┘
                      │
    ┌─────────────────┼─────────────────┐
    │                 │                 │
┌──────────────┐ ┌──────────────┐ ┌──────────────┐
│    MADIS     │ │    Other     │ │    Other     │
│   Ingestor   │ │  Ingestor A  │ │  Ingestor B  │
└──────┬───────┘ └──────┬───────┘ └──────┬───────┘
       │                │                │
       └───────────── publish normalized events ───────┐
                                                       │
                                         ┌─────────────▼─────────────┐
                                         │      Kalshi Trader        │
                                         │     (multi-market)        │
                                         └─────────────┬─────────────┘
                                                       │
                              ┌─────────────────────────┼─────────────────────────┐
                              │                         │                         │
                    ┌─────────▼─────────┐    ┌─────────▼─────────┐    ┌──────────▼──────────┐
                    │  Kalshi REST API  │    │ Kalshi WebSocket  │    │   Market Discovery  │
                    │  (order placement)│    │   (orderbook)     │    │    (on startup)     │
                    └───────────────────┘    └───────────────────┘    └─────────────────────┘
```

---

## Core Design Principles

1. **Event-driven**: data arrives → trader reacts
2. **Single trader process** (initially):
   - Centralized risk
   - Shared market data
   - One reconciliation loop
3. **Multiple ingestors** publish into a shared bus
4. **Strict normalization**: trader never cares about raw source formats
5. **Idempotency everywhere**: no duplicate actions
6. **Fail safe**: stale or malformed data never causes trades
7. **Runtime discovery**: trader discovers available markets at startup

---

## Event Bus: NATS

### Why NATS
- Extremely low latency
- Simple pub/sub semantics
- Easy fan-in from many ingestors
- No heavy ops burden
- Natural upgrade path to JetStream if durability is needed later

### Subject Naming

#### Observation events
```
obs.metar.KMDW
obs.metar.KMIA
```

Later expansion examples:
```
obs.wind.KMDW
obs.temp.KMIA
```

#### Trader output (optional but recommended)
```
trade.intent
trade.order
trade.fill
trade.risk
```

#### System / control
```
sys.health.<service>
sys.kill
```

---

## Normalized Observation Event Schema

All ingestors must publish a normalized payload so the trader is source-agnostic.

### ObservationEvent (conceptual)
- `event_type`: `"metar_observation"`
- `source`: `"madis_hfmetar"`
- `station`: `"KMDW"`
- `obs_time_utc`: timestamp from data
- `recv_time_utc`: local receipt time
- `raw_metar`: full METAR string
- `fields` (parsed):
  - `temp_c`
  - `wind_speed_kt` (optional)
  - `gust_kt` (optional)
  - `visibility_sm` (optional)
- `dedupe_key`:
  - stable hash of `(source, station, obs_time_utc, raw_metar)`

---

## MADIS Ingestor Architecture

### Responsibilities
- Poll hourly MADIS hfmetar netCDF files
- Detect new publication via HEAD + ETag / Last-Modified
- Download only when changed
- Parse netCDF once per file
- Extract latest record per configured station
- Emit one ObservationEvent per station

### Multi-station Handling
- Configured station list, e.g.: `[KMDW, KMIA, KORD]`
- For each station:
  - select record with max `timeObs`
  - dedupe using last emitted `(station, obs_time)`

### Polling Strategy
- Interval: **2–5 seconds** (for time-sensitive trading)
- Candidate files:
  - current UTC hour
  - previous 1–2 hours
- Conditional GET with ETag to minimize bandwidth

---

## Multiple Ingestors

The system is explicitly designed for **many independent ingestors**.

Examples:
- MADIS METAR ingestor
- Alternate weather API ingestor
- Private sensor feed
- Derived signal service

All ingestors:
- Publish normalized events
- Do **no trading logic**
- Are stateless beyond dedupe

---

## Kalshi Trader Architecture

### Single Trader Process (Recommended)

Handles:
- Multiple stations
- Multiple markets per station
- Multiple strategies
- One portfolio
- One risk manager

### Internal Structure

```
Kalshi Trader
├── Market Discovery (startup)
│   └── REST API: GET /markets?status=open
├── WebSocket Manager
│   ├── Orderbook subscriptions
│   └── Fill notifications
├── NATS Subscriber
│   └── obs.metar.* events
├── Location Engines
│   ├── Chicago Engine
│   │   ├── High Temp Buckets
│   │   └── Low Temp Buckets
│   └── Miami Engine
│       ├── High Temp Buckets
│       └── Low Temp Buckets
├── Risk Manager (global)
└── Order Manager / Reconciliation
```

---

## Market Discovery (Runtime)

### Startup Flow
1. **REST API**: `GET /markets?status=open&limit=200`
2. **Filter**: tickers starting with `KXLOWT` or `KXHIGH`
3. **Parse**: extract city code, bucket range from ticker/title
4. **Map**: city code → weather station (e.g., `CHI` → `KMDW`)
5. **Subscribe**: WebSocket orderbook for all discovered tickers
6. **Cache**: store bucket info for trading logic

### City → Station Mapping
```
CHI → KMDW (Chicago Midway)
MIA → KMIA (Miami International)
DEN → KDEN (Denver)
NYC → KJFK (JFK)
ATL → KATL (Atlanta)
...
```

### Bucket Parsing
From ticker `KXLOWTCHI-26JAN15-B52`:
- Series: `KXLOWTCHI`
- Type: `LOWT` = low temperature
- City: `CHI` = Chicago
- Date: `26JAN15`
- Bucket: `B52` = 52°F lower bound

---

## Kalshi WebSocket Integration

### Purpose
- **Dead bucket detection**: skip markets with no liquidity
- **Real-time orderbook**: know best bid/ask before ordering
- **Fill notifications**: immediate confirmation

### Connection
```
Production: wss://api.elections.kalshi.com/trade-api/ws/v2
Demo: wss://demo-api.kalshi.co/trade-api/ws/v2
```

### Subscriptions
- `orderbook_delta` for each discovered market ticker
- `fills` for order confirmation

### Orderbook Cache
```cpp
struct Orderbook {
    std::map<int, int> yes_bids;  // price → quantity
    std::map<int, int> no_asks;

    bool has_liquidity() const {
        return !no_asks.empty() || !yes_bids.empty();
    }
};
```

---

## Strategy Model (Reactive Only)

- No forecasting
- No stateful prediction
- Strategy reacts immediately to incoming observations

### Trading Rules

**LOW Temperature Markets**:
- When observation drops below bucket range → Buy NO on all buckets above
- Example: Reading 51°F → Buy NO on 52-53, 53-54, 54-55, etc.

**HIGH Temperature Markets**:
- When observation rises above bucket range → Buy NO on all buckets below
- Example: Reading 76°F → Buy NO on 72-73, 73-74, 74-75, etc.

### General Flow
1. ObservationEvent arrives (via NATS)
2. Convert temp: Celsius → Fahrenheit
3. Identify affected buckets for that city
4. For each qualifying bucket:
   - Check WebSocket orderbook for liquidity
   - Skip dead buckets
   - Place order if liquid
5. Mark bucket as traded (no duplicates)

---

## Risk Management (Mandatory)

### Global Limits
- Max total exposure
- Max daily loss
- Max open orders
- Max orders per minute

### Per-Market Limits
- Max position
- Max order size

### Data Sanity Guards
- Ignore stale observations (> N minutes old)
- Ignore backward timestamps
- Ignore malformed or partial events
- **Skip dead buckets** (no liquidity)

### Kill Switch
- NATS subject: `sys.kill`
- Or config flag / file
- Immediately disables new orders

---

## Execution & Order Management

### Order Policy
- Compute desired order set per market
- Compare vs current open orders
- Cancel/replace only if meaningfully different
- Enforce minimum time between updates

### Idempotency
- Orders tied to `(market_id, event_dedupe_key, strategy_version)`
- Retries never duplicate intent
- `traded` flag per bucket prevents re-ordering

### Reconciliation
- Periodic refresh from Kalshi:
  - open orders
  - positions
- Repair divergences automatically

---

## Persistence (Optional)

For the reactive trading strategy, persistence is **not required**:
- Markets are discovered fresh on each startup
- Bucket `traded` flags reset on restart (safe: worst case is re-placing orders)
- Positions can be queried from Kalshi API

If persistence is desired later (for auditing/debugging):
- Lightweight SQLite or simple log files
- Order history for post-trade analysis
- Not on the critical trading path

---

## Observability

### Metrics
- Event receive → order submit latency
- Publish detection delay
- Reject rates
- Exposure by market / city
- Dead bucket skip rate

### Logging
Structured logs with:
- `event_id`
- `station`
- `obs_time`
- `market_id`
- `order_id`
- `liquidity_status`

### Alerts
- No new data for N minutes
- Parse failures
- Repeated order rejects
- Risk kill switch triggered
- WebSocket disconnection

---

## Deployment

### Initial Deployment
- Single machine
- `nats-server`
- Multiple ingestors
- One trader
- Managed by systemd

### Hardening
- Restart policies
- Resource limits
- Separate credentials (ingestors never have trading keys)

### Future Scaling
- Split trader by city if isolation needed
- Add JetStream for replay
- Run trader closer to Kalshi endpoints

---

## Design Decision Summary

| Decision | Choice |
|----------|--------|
| Event bus | NATS |
| Trader model | One trader, multi-market |
| Ingestors | Many, independent |
| Strategy | Reactive only |
| Forecasting | None |
| Market discovery | Runtime (REST API at startup) |
| Orderbook | WebSocket streaming |
| Dead bucket handling | Skip (no order) |
| Sharding | By city (later, if needed) |

---

## Implementation Phases

### Phase 1: Foundation
- NATS server setup
- MADIS ingestor (standalone)
- Basic trader subscribing to NATS

### Phase 2: Market Integration
- REST API market discovery
- City → station mapping
- Bucket parsing

### Phase 3: WebSocket
- Kalshi WebSocket connection
- Orderbook caching
- Dead bucket detection

### Phase 4: Trading Logic
- Reactive strategy implementation
- Order placement
- Risk management

### Phase 5: Hardening
- Reconciliation (query Kalshi for positions)
- Observability (logging, metrics)
- Optional: order history logging

---

## Final Recommendation

This architecture:
- Scales cleanly from 1 → many markets
- Minimizes latency while preserving safety
- Keeps complexity out of the trading hot path
- Automatically adapts to available markets via runtime discovery
- Avoids wasted orders on dead buckets

It is an excellent fit for **event-driven Kalshi trading based on real-time observations**.
