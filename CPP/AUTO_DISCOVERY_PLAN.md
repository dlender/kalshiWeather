# Automatic Market Discovery & Dynamic Strategy Plan

## Overview
Automatically discover Kalshi weather markets and create dynamic trading strategies based on a simple rule:
- **LOW temp markets**: When reading drops below a bucket, buy NO on ALL buckets above the reading
- **HIGH temp markets**: When reading rises above a bucket, buy NO on ALL buckets below the reading
- **Dead bucket detection**: Use websocket orderbook to skip buckets with no liquidity

## Strategy Logic

### Low Temperature Markets (e.g., KXLOWTCHI)
```
Buckets available: 52-53, 53-54, 54-55, 55-56
Current reading: 51°F (10.5°C)

→ Buy NO on: 52-53, 53-54, 54-55, 55-56 (all buckets above 51)
   Reasoning: Actual low will be ≤51, so it can't be in 52-53 or higher
```

### High Temperature Markets (e.g., KXHIGHCHI)
```
Buckets available: 72-73, 73-74, 74-75, 75-76
Current reading: 76°F (24.4°C)

→ Buy NO on: 72-73, 73-74, 74-75 (all buckets below 76)
   Reasoning: Actual high will be ≥76, so it can't be in 72-73 or lower
```

## Implementation

### 1. Add City/Station Mapping
```cpp
const std::unordered_map<std::string, std::string> CITY_STATION_MAP = {
    {"CHI", "KMDW"},   // Chicago
    {"MIA", "KMIA"},   // Miami
    {"DEN", "KDEN"},   // Denver
    {"NYC", "KJFK"},   // New York
    {"ATL", "KATL"},   // Atlanta
    // Expand as needed
};
```

### 2. Market Discovery Structs
```cpp
struct BucketInfo {
    std::string ticker;        // "KXLOWTCHI-26JAN15-B52"
    int low_f;                 // 52 (bucket low bound in °F)
    int high_f;                // 53 (bucket high bound in °F)
    int low_c;                 // Converted to Celsius
    int high_c;
    bool traded = false;       // Already placed order?
};

struct CityMarkets {
    std::string city_code;     // "CHI"
    std::string station;       // "KMDW"
    std::vector<BucketInfo> low_buckets;   // Low temp market buckets
    std::vector<BucketInfo> high_buckets;  // High temp market buckets
};
```

### 3. Market Discovery Function
```cpp
std::vector<CityMarkets> discover_all_markets() {
    // 1. GET /markets?status=open&limit=200
    // 2. Filter for tickers starting with "KXLOWT" and "KXHIGH"
    // 3. Group by city code
    // 4. Parse bucket ranges from tickers/titles
    // 5. Sort buckets by temperature
    // 6. Return organized structure
}
```

### 4. Dynamic Trading Logic
```cpp
void check_and_trade(CityMarkets& city,
                     int current_temp_c,
                     KalshiClient& client) {
    int current_temp_f = c_to_f(current_temp_c);

    // LOW TEMP: Buy NO on all buckets ABOVE current reading
    for (auto& bucket : city.low_buckets) {
        if (!bucket.traded && bucket.low_f > current_temp_f) {
            client.buy(bucket.ticker, 5, Side::NO, 99);
            bucket.traded = true;
            std::cout << "LOW: Temp " << current_temp_f << "°F → Buy NO on "
                      << bucket.ticker << " (" << bucket.low_f << "-" << bucket.high_f << ")\n";
        }
    }

    // HIGH TEMP: Buy NO on all buckets BELOW current reading
    for (auto& bucket : city.high_buckets) {
        if (!bucket.traded && bucket.high_f < current_temp_f) {
            client.buy(bucket.ticker, 5, Side::NO, 99);
            bucket.traded = true;
            std::cout << "HIGH: Temp " << current_temp_f << "°F → Buy NO on "
                      << bucket.ticker << " (" << bucket.low_f << "-" << bucket.high_f << ")\n";
        }
    }
}
```

### 5. Updated Main Loop
```cpp
// At startup:
std::cout << "Discovering markets...\n";
auto all_markets = kalshi_client.discover_all_markets();
for (const auto& city : all_markets) {
    std::cout << city.city_code << " (" << city.station << "): "
              << city.low_buckets.size() << " low buckets, "
              << city.high_buckets.size() << " high buckets\n";
}

// In polling loop:
for (auto& city : all_markets) {
    auto obs = madis_client.fetch_latest(city.station, madis_states[city.station]);
    if (obs.is_new_data && obs.obs->valid) {
        check_and_trade(city, obs.obs->temp_c, kalshi_client);
    }
}
```

### 6. Config Options
```cpp
struct TradingConfig {
    int poll_interval_sec = 2;
    bool verbose = false;
    bool auto_discover = true;  // Enable market discovery
    int default_contracts = 5;  // Contracts per trade
    int default_price = 99;     // Price in cents
};
```

## Temperature Conversion
Use asos_lookup_table.csv for accurate C↔F conversion accounting for rounding:
```cpp
int c_to_f(int celsius) {
    // Simple: return celsius * 9/5 + 32
    // Or use lookup table for precise bucket matching
}
```

## Files to Modify
- `weatherTrader.cpp` - Complete refactor of strategy system

## New .env Options
```
AUTO_DISCOVER=true    # Enable automatic market discovery
DEFAULT_CONTRACTS=5   # Contracts per trade
DEFAULT_PRICE=99      # Price in cents (0-99)
VERBOSE=true          # Show detailed logs
```

## Implementation Order
1. Add temperature conversion function (C↔F)
2. Add CITY_STATION_MAP
3. Add BucketInfo and CityMarkets structs
4. Add discover_all_markets() to KalshiClient
5. Add check_and_trade() function
6. Refactor main loop to use dynamic markets
7. Remove hardcoded TradingStrategy configuration

## WebSocket Integration (Dead Bucket Detection)

### Why WebSocket?
- Avoid submitting orders to "dead" buckets (no liquidity)
- Get real-time orderbook updates instead of polling
- Instant fill notifications

### WebSocket Architecture
```
┌─────────────────────────────────────────────────────────────┐
│                        STARTUP                               │
├─────────────────────────────────────────────────────────────┤
│ 1. REST API: Discover all weather markets                   │
│ 2. WebSocket: Connect and subscribe to orderbook channels   │
│ 3. WebSocket: Receive initial orderbook snapshots           │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      RUNTIME (parallel)                      │
├─────────────────────────────────────────────────────────────┤
│ Thread 1: MADIS polling (weather data)                      │
│ Thread 2: WebSocket listener (orderbook updates)            │
│ Thread 3: Trading logic (check triggers, place orders)      │
└─────────────────────────────────────────────────────────────┘
```

### WebSocket Connection
```cpp
// Production: wss://api.elections.kalshi.com/trade-api/ws/v2
// Demo: wss://demo-api.kalshi.co/trade-api/ws/v2

class KalshiWebSocket {
    void connect();
    void authenticate();  // Same RSA-PSS signing as REST
    void subscribe_orderbook(const std::vector<std::string>& tickers);
    void on_orderbook_snapshot(const OrderbookSnapshot& snapshot);
    void on_orderbook_delta(const OrderbookDelta& delta);
};
```

### Orderbook Tracking
```cpp
struct Orderbook {
    std::string ticker;
    std::map<int, int> yes_bids;  // price → quantity
    std::map<int, int> yes_asks;
    std::map<int, int> no_bids;
    std::map<int, int> no_asks;

    bool has_liquidity() const {
        // Check if there are any resting orders
        return !no_asks.empty() || !yes_bids.empty();
    }

    int best_no_ask() const {
        return no_asks.empty() ? -1 : no_asks.begin()->first;
    }
};

// Global orderbook cache updated by websocket
std::unordered_map<std::string, Orderbook> orderbooks;
```

### Updated BucketInfo
```cpp
struct BucketInfo {
    std::string ticker;
    int low_f, high_f;
    int low_c, high_c;
    bool traded = false;

    // NEW: Check orderbook before trading
    bool has_liquidity() const {
        auto it = orderbooks.find(ticker);
        return it != orderbooks.end() && it->second.has_liquidity();
    }
};
```

### Updated Trading Logic
```cpp
void check_and_trade(CityMarkets& city, int current_temp_c, KalshiClient& client) {
    int current_temp_f = c_to_f(current_temp_c);

    for (auto& bucket : city.low_buckets) {
        if (!bucket.traded && bucket.low_f > current_temp_f) {
            // NEW: Skip dead buckets
            if (!bucket.has_liquidity()) {
                std::cout << "SKIP (dead): " << bucket.ticker << "\n";
                continue;
            }

            client.buy(bucket.ticker, 5, Side::NO, 99);
            bucket.traded = true;
        }
    }
    // ... same for high_buckets
}
```

### WebSocket Message Handling
```cpp
void on_message(const std::string& msg) {
    auto json = parse_json(msg);

    if (json["type"] == "orderbook_snapshot") {
        // Full orderbook state
        Orderbook ob;
        ob.ticker = json["market_ticker"];
        for (auto& level : json["yes"]) {
            ob.yes_bids[level["price"]] = level["quantity"];
        }
        // ... parse all levels
        orderbooks[ob.ticker] = ob;
    }
    else if (json["type"] == "orderbook_delta") {
        // Incremental update
        auto& ob = orderbooks[json["market_ticker"]];
        int price = json["price"];
        int delta = json["delta"];  // can be negative
        ob.no_asks[price] += delta;
        if (ob.no_asks[price] <= 0) {
            ob.no_asks.erase(price);
        }
    }
}
```

## C++ WebSocket Library Options
- **libwebsockets** - Low-level, high performance
- **Boost.Beast** - Part of Boost, well-maintained
- **websocketpp** - Header-only, easy to integrate
- **IXWebSocket** - Simple, modern C++

## Implementation Order (Updated)
1. Add temperature conversion function (C↔F)
2. Add CITY_STATION_MAP
3. Add BucketInfo and CityMarkets structs
4. Add discover_all_markets() to KalshiClient (REST)
5. **Add WebSocket client for orderbook streaming**
6. **Add Orderbook struct and global cache**
7. **Add has_liquidity() check to trading logic**
8. Add check_and_trade() function with dead bucket skip
9. Refactor main loop with websocket thread

## New Dependencies
```bash
# For websocket support (choose one):
sudo apt install libwebsockets-dev     # libwebsockets
# OR
sudo apt install libboost-all-dev      # Boost.Beast
```

## Verification
1. Build and run with `VERBOSE=true AUTO_DISCOVER=true`
2. Verify markets are discovered for all cities
3. Verify websocket connects and receives orderbook snapshots
4. Verify buckets are correctly parsed and sorted
5. Verify dead buckets are skipped (no orders placed)
6. Verify trades trigger correctly on live buckets:
   - Low temp: NO orders placed on buckets above reading
   - High temp: NO orders placed on buckets below reading
7. Verify trades are not duplicated (traded flag works)
