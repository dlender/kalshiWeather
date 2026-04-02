# Weather Trader v2 - NATS-Based Architecture

Event-driven weather trading system with automatic market discovery.

## Features
- **Automatic market discovery** via Kalshi REST API
- **WebSocket orderbook** for dead bucket detection
- **NATS event bus** for decoupled ingestor/trader architecture
- **Dynamic strategy generation** based on discovered markets

## Architecture

```
             +---------------+
             |  nats-server  |
             +-------+-------+
                     |
    +----------------+----------------+
    |                                 |
+---+---+                        +----+----+
| MADIS |   publish obs.metar.*  | Kalshi  |
|Ingest.|----------------------->| Trader  |
+-------+                        +----+----+
                                      |
                    +-----------------+-----------------+
                    |                 |                 |
              +-----+-----+     +-----+-----+     +-----+-----+
              |  REST API |     | WebSocket |     |   Market  |
              |  (orders) |     |(orderbook)|     | Discovery |
              +-----------+     +-----------+     +-----------+
```

## Build

```bash
# Install dependencies (Ubuntu/WSL)
sudo apt install -y build-essential libcurl4-openssl-dev libssl-dev \
    zlib1g-dev libnetcdf-dev libnats-c-dev

# Build
make

# Or with CMake
mkdir build && cd build
cmake .. && make
```

## Run

```bash
# Start NATS server (separate terminal)
nats-server

# Run ingestor (separate terminal)
./madis_ingestor

# Run trader
./kalshi_trader
```

## Configuration (.env)

```
KALSHI_API_KEY_ID=your-api-key
KALSHI_PRIVATE_KEY=path/to/private.pem
KALSHI_ENV=demo           # or "prod"
NATS_URL=nats://localhost:4222
AUTO_DISCOVER=true
DEFAULT_CONTRACTS=5
DEFAULT_PRICE=99
VERBOSE=true
```
