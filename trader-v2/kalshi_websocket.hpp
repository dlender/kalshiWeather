// kalshi_websocket.hpp
// WebSocket client for Kalshi orderbook streaming
//
// Uses libwebsockets for WebSocket connectivity.
// Provides real-time orderbook updates for dead bucket detection.

#pragma once

#include "common.hpp"
#include "trading_logic.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Note: This is a stub implementation that can work without libwebsockets.
// For full WebSocket support, you'll need to install libwebsockets:
//   sudo apt install libwebsockets-dev
// And link with -lwebsockets

namespace kalshi_ws {

// ============================================================================
// SIMPLE JSON PARSER (no external dependencies)
// ============================================================================

class JsonParser {
public:
    static std::string get_string(const std::string& json, const std::string& key) {
        std::string needle = "\"" + key + "\":\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return "";
        pos += needle.size();
        auto end = json.find("\"", pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    }

    static int get_int(const std::string& json, const std::string& key) {
        std::string needle = "\"" + key + "\":";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return 0;
        pos += needle.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

        bool negative = false;
        if (pos < json.size() && json[pos] == '-') {
            negative = true;
            pos++;
        }

        int val = 0;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            val = val * 10 + (json[pos] - '0');
            pos++;
        }
        return negative ? -val : val;
    }

    static std::string get_type(const std::string& json) {
        return get_string(json, "type");
    }
};

// ============================================================================
// ORDERBOOK MESSAGE TYPES
// ============================================================================

struct OrderbookSnapshot {
    std::string ticker;
    std::vector<std::pair<int, int>> yes_bids;  // price, quantity
    std::vector<std::pair<int, int>> yes_asks;
    std::vector<std::pair<int, int>> no_bids;
    std::vector<std::pair<int, int>> no_asks;
};

struct OrderbookDelta {
    std::string ticker;
    std::string side;   // "yes" or "no"
    std::string action; // "bid" or "ask"
    int price;
    int delta;  // Can be negative
};

// ============================================================================
// WEBSOCKET CLIENT (Stub implementation - works without external deps)
// ============================================================================

class KalshiWebSocket {
public:
    using MessageCallback = std::function<void(const std::string&)>;

    KalshiWebSocket(const std::string& ws_url,
                    const std::string& api_key,
                    const std::string& timestamp,
                    const std::string& signature)
        : ws_url_(ws_url)
        , api_key_(api_key)
        , timestamp_(timestamp)
        , signature_(signature)
        , connected_(false)
        , running_(false) {}

    ~KalshiWebSocket() {
        stop();
    }

    void set_message_callback(MessageCallback cb) {
        message_callback_ = cb;
    }

    // Subscribe to orderbook for specific tickers
    void subscribe_orderbook(const std::vector<std::string>& tickers) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_subscriptions_.insert(pending_subscriptions_.end(),
                                       tickers.begin(), tickers.end());
    }

    // Start WebSocket connection in background thread
    bool start() {
        if (running_) return true;
        running_ = true;

        // In a real implementation, this would:
        // 1. Connect to wss://... endpoint
        // 2. Send authentication with API key, timestamp, signature
        // 3. Subscribe to orderbook channels
        // 4. Process incoming messages

        // For now, this is a stub that simulates WebSocket behavior
        worker_thread_ = std::thread([this]() {
            std::cout << "[WebSocket] Stub mode - no real connection\n";
            std::cout << "[WebSocket] Install libwebsockets for real orderbook streaming\n";

            // Simulate connection delay
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            connected_ = true;

            while (running_) {
                // In real implementation, would read messages here
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });

        return true;
    }

    void stop() {
        running_ = false;
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    bool is_connected() const { return connected_; }

    // Process an incoming message (for manual integration)
    void process_message(const std::string& msg) {
        std::string type = JsonParser::get_type(msg);

        if (type == "orderbook_snapshot") {
            process_snapshot(msg);
        } else if (type == "orderbook_delta") {
            process_delta(msg);
        }

        if (message_callback_) {
            message_callback_(msg);
        }
    }

private:
    std::string ws_url_;
    std::string api_key_;
    std::string timestamp_;
    std::string signature_;

    std::atomic<bool> connected_;
    std::atomic<bool> running_;
    std::thread worker_thread_;

    std::mutex mutex_;
    std::vector<std::string> pending_subscriptions_;
    MessageCallback message_callback_;

    void process_snapshot(const std::string& msg) {
        std::string ticker = JsonParser::get_string(msg, "market_ticker");
        if (ticker.empty()) return;

        trading::Orderbook ob;
        ob.ticker = ticker;

        // In real implementation, would parse full orderbook levels
        // For now, just mark that we have data
        ob.last_update = std::time(nullptr);

        // Check if there's liquidity mentioned in the message
        // Look for "yes" or "no" arrays with content
        if (msg.find("\"yes\":[{") != std::string::npos ||
            msg.find("\"no\":[{") != std::string::npos) {
            // Has some orders - add a dummy level to indicate liquidity
            ob.yes_bids.push_back({50, 10});
        }

        trading::g_orderbooks.update(ticker, ob);
    }

    void process_delta(const std::string& msg) {
        std::string ticker = JsonParser::get_string(msg, "market_ticker");
        if (ticker.empty()) return;

        // Get existing orderbook or create new one
        auto existing = trading::g_orderbooks.get(ticker);
        trading::Orderbook ob = existing.value_or(trading::Orderbook{ticker});

        // Parse delta
        std::string side = JsonParser::get_string(msg, "side");
        int price = JsonParser::get_int(msg, "price");
        int delta = JsonParser::get_int(msg, "delta");

        // Apply delta (simplified - real implementation would be more complete)
        if (side == "yes" && delta > 0) {
            ob.yes_bids.push_back({price, delta});
        } else if (side == "no" && delta > 0) {
            ob.no_asks.push_back({price, delta});
        }

        ob.last_update = std::time(nullptr);
        trading::g_orderbooks.update(ticker, ob);
    }
};

// ============================================================================
// HELPER: Build subscription message
// ============================================================================

inline std::string build_subscribe_message(const std::vector<std::string>& tickers) {
    std::ostringstream oss;
    oss << R"({"id":1,"cmd":"subscribe","params":{"channels":["orderbook_delta"],"market_tickers":[)";

    bool first = true;
    for (const auto& ticker : tickers) {
        if (!first) oss << ",";
        oss << "\"" << ticker << "\"";
        first = false;
    }

    oss << "]}}";
    return oss.str();
}

// ============================================================================
// HELPER: Simulate orderbook with initial liquidity
// ============================================================================

// Call this to pre-populate orderbooks with simulated liquidity
// (useful when WebSocket is not available)
inline void simulate_liquidity(const std::vector<std::string>& tickers, bool has_liquidity = true) {
    for (const auto& ticker : tickers) {
        trading::Orderbook ob;
        ob.ticker = ticker;
        ob.last_update = std::time(nullptr);

        if (has_liquidity) {
            // Add dummy liquidity
            ob.yes_bids.push_back({50, 100});
            ob.no_asks.push_back({50, 100});
        }

        trading::g_orderbooks.update(ticker, ob);
    }
}

} // namespace kalshi_ws
