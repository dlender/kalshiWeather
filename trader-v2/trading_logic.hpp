// trading_logic.hpp
// Dynamic trading logic based on discovered markets

#pragma once

#include "common.hpp"
#include "kalshi_client.hpp"

#include <iostream>
#include <mutex>
#include <unordered_map>

namespace trading {

// ============================================================================
// ORDERBOOK CACHE (updated by WebSocket)
// ============================================================================

struct OrderbookLevel {
    int price;      // 1-99 cents
    int quantity;   // Number of contracts
};

struct Orderbook {
    std::string ticker;
    std::vector<OrderbookLevel> yes_bids;  // Sorted by price descending
    std::vector<OrderbookLevel> yes_asks;  // Sorted by price ascending
    std::vector<OrderbookLevel> no_bids;   // Sorted by price descending
    std::vector<OrderbookLevel> no_asks;   // Sorted by price ascending
    std::time_t last_update = 0;

    bool has_liquidity() const {
        // Check if there are any resting orders we could trade against
        // For buying NO, we need either no_asks or yes_bids
        return !no_asks.empty() || !yes_bids.empty();
    }

    int best_no_ask() const {
        return no_asks.empty() ? -1 : no_asks.front().price;
    }

    int best_yes_bid() const {
        return yes_bids.empty() ? -1 : yes_bids.front().price;
    }

    int total_liquidity() const {
        int total = 0;
        for (const auto& level : yes_bids) total += level.quantity;
        for (const auto& level : no_asks) total += level.quantity;
        return total;
    }
};

// Global orderbook cache (thread-safe)
class OrderbookCache {
public:
    void update(const std::string& ticker, const Orderbook& ob) {
        std::lock_guard<std::mutex> lock(mutex_);
        orderbooks_[ticker] = ob;
        orderbooks_[ticker].last_update = std::time(nullptr);
    }

    std::optional<Orderbook> get(const std::string& ticker) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = orderbooks_.find(ticker);
        if (it == orderbooks_.end()) return std::nullopt;
        return it->second;
    }

    bool has_liquidity(const std::string& ticker) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = orderbooks_.find(ticker);
        return it != orderbooks_.end() && it->second.has_liquidity();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Orderbook> orderbooks_;
};

// Global instance
inline OrderbookCache g_orderbooks;

// ============================================================================
// TRADING CONFIG
// ============================================================================

struct TradingConfig {
    int default_contracts = 5;   // Contracts per trade
    int default_price = 99;      // Limit price in cents
    bool verbose = false;
    bool skip_dead_buckets = true;  // Skip buckets with no liquidity
    int stale_data_threshold_sec = 300;  // Ignore data older than 5 minutes
};

// ============================================================================
// CHECK AND TRADE FUNCTION
// ============================================================================

// Check temperature against buckets and place orders
// Returns number of orders placed
inline int check_and_trade(CityMarkets& city,
                           int current_temp_c,
                           kalshi::KalshiClient& client,
                           const TradingConfig& config) {
    int current_temp_f = c_to_f(current_temp_c);
    int orders_placed = 0;

    // LOW TEMP: Buy NO on all buckets ABOVE current reading
    // Reasoning: If current temp is 51°F, the low will be ≤51°F,
    //            so it CAN'T be in bucket 52-53 or higher
    for (auto& bucket : city.low_buckets) {
        if (bucket.traded) continue;  // Already traded

        // Skip buckets at or below current temp
        if (bucket.low_f <= current_temp_f) continue;

        // Check liquidity if enabled
        if (config.skip_dead_buckets) {
            auto ob = g_orderbooks.get(bucket.ticker);
            if (!ob || !ob->has_liquidity()) {
                if (config.verbose) {
                    std::cout << "[SKIP] " << bucket.ticker
                              << " - no liquidity (LOW, bucket " << bucket.low_f
                              << "°F > current " << current_temp_f << "°F)\n";
                }
                continue;
            }
        }

        // Place order
        std::cout << "\n*** PLACING ORDER ***\n";
        std::cout << "Strategy: LOW TEMP - temp dropped below bucket\n";
        std::cout << "City: " << city.city_code << " (" << city.station << ")\n";
        std::cout << "Current temp: " << current_temp_f << "°F (" << current_temp_c << "°C)\n";
        std::cout << "Bucket: " << bucket.low_f << "-" << bucket.high_f << "°F\n";
        std::cout << "Action: BUY " << config.default_contracts << " NO @ "
                  << config.default_price << "c on " << bucket.ticker << "\n";

        auto result = client.buy(bucket.ticker, config.default_contracts,
                                  kalshi::Side::NO, config.default_price);

        if (result.success) {
            std::cout << "ORDER PLACED! Order ID: " << result.order_id << "\n";
            bucket.traded = true;
            orders_placed++;
        } else {
            std::cout << "ORDER FAILED! Status: " << result.status_code << "\n";
            std::cout << "Response: " << result.raw_response << "\n";
        }
        std::cout << "\n";
    }

    // HIGH TEMP: Buy NO on all buckets BELOW current reading
    // Reasoning: If current temp is 76°F, the high will be ≥76°F,
    //            so it CAN'T be in bucket 72-73 or lower
    for (auto& bucket : city.high_buckets) {
        if (bucket.traded) continue;  // Already traded

        // Skip buckets at or above current temp
        if (bucket.high_f >= current_temp_f) continue;

        // Check liquidity if enabled
        if (config.skip_dead_buckets) {
            auto ob = g_orderbooks.get(bucket.ticker);
            if (!ob || !ob->has_liquidity()) {
                if (config.verbose) {
                    std::cout << "[SKIP] " << bucket.ticker
                              << " - no liquidity (HIGH, bucket " << bucket.high_f
                              << "°F < current " << current_temp_f << "°F)\n";
                }
                continue;
            }
        }

        // Place order
        std::cout << "\n*** PLACING ORDER ***\n";
        std::cout << "Strategy: HIGH TEMP - temp rose above bucket\n";
        std::cout << "City: " << city.city_code << " (" << city.station << ")\n";
        std::cout << "Current temp: " << current_temp_f << "°F (" << current_temp_c << "°C)\n";
        std::cout << "Bucket: " << bucket.low_f << "-" << bucket.high_f << "°F\n";
        std::cout << "Action: BUY " << config.default_contracts << " NO @ "
                  << config.default_price << "c on " << bucket.ticker << "\n";

        auto result = client.buy(bucket.ticker, config.default_contracts,
                                  kalshi::Side::NO, config.default_price);

        if (result.success) {
            std::cout << "ORDER PLACED! Order ID: " << result.order_id << "\n";
            bucket.traded = true;
            orders_placed++;
        } else {
            std::cout << "ORDER FAILED! Status: " << result.status_code << "\n";
            std::cout << "Response: " << result.raw_response << "\n";
        }
        std::cout << "\n";
    }

    return orders_placed;
}

// ============================================================================
// PRINT MARKET SUMMARY
// ============================================================================

inline void print_market_summary(const std::vector<CityMarkets>& markets) {
    std::cout << "=================================================\n";
    std::cout << "  DISCOVERED MARKETS\n";
    std::cout << "=================================================\n\n";

    int total_low = 0, total_high = 0;

    for (const auto& city : markets) {
        std::cout << city.city_code << " (" << city.station << ")";
        if (!city.date_suffix.empty()) {
            std::cout << " - " << city.date_suffix;
        }
        std::cout << "\n";

        std::cout << "  LOW buckets: " << city.low_buckets.size();
        if (!city.low_buckets.empty()) {
            std::cout << " (" << city.low_buckets.front().low_f << "°F - "
                      << city.low_buckets.back().high_f << "°F)";
        }
        std::cout << "\n";

        std::cout << "  HIGH buckets: " << city.high_buckets.size();
        if (!city.high_buckets.empty()) {
            std::cout << " (" << city.high_buckets.front().low_f << "°F - "
                      << city.high_buckets.back().high_f << "°F)";
        }
        std::cout << "\n\n";

        total_low += city.low_buckets.size();
        total_high += city.high_buckets.size();
    }

    std::cout << "Total: " << markets.size() << " cities, "
              << total_low << " low buckets, " << total_high << " high buckets\n\n";
}

// Get all tickers from discovered markets (for WebSocket subscription)
inline std::vector<std::string> get_all_tickers(const std::vector<CityMarkets>& markets) {
    std::vector<std::string> tickers;
    for (const auto& city : markets) {
        for (const auto& bucket : city.low_buckets) {
            tickers.push_back(bucket.ticker);
        }
        for (const auto& bucket : city.high_buckets) {
            tickers.push_back(bucket.ticker);
        }
    }
    return tickers;
}

} // namespace trading
