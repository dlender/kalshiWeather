// kalshi_trader.cpp
//
// Weather trading system with automatic market discovery.
//
// Features:
// - Auto-discovers weather markets at startup via Kalshi REST API
// - Polls MADIS METAR data for all cities with active markets
// - Places orders based on reactive strategy:
//   - LOW temp: Buy NO on buckets above current reading
//   - HIGH temp: Buy NO on buckets below current reading
// - WebSocket orderbook for dead bucket detection (optional)
//
// Build:
//   g++ -O2 -std=c++20 kalshi_trader.cpp -lcurl -lssl -lcrypto -lz -lnetcdf -o kalshi_trader
//
// Run:
//   ./kalshi_trader
//
// Configuration (.env):
//   KALSHI_API_KEY_ID    - Your API key ID
//   KALSHI_PRIVATE_KEY   - Path to RSA private key PEM
//   KALSHI_ENV           - "demo" or "prod"
//   AUTO_DISCOVER        - "true" to discover markets at startup
//   DEFAULT_CONTRACTS    - Contracts per trade (default: 5)
//   DEFAULT_PRICE        - Limit price in cents (default: 99)
//   VERBOSE              - "true" for verbose logging

#include "common.hpp"
#include "madis_client.hpp"
#include "kalshi_client.hpp"
#include "trading_logic.hpp"

#include <curl/curl.h>

#include <chrono>
#include <csignal>
#include <iostream>
#include <set>
#include <thread>

// ============================================================================
// SIGNAL HANDLING
// ============================================================================

static volatile bool g_running = true;

void signal_handler(int signum) {
    std::cout << "\nReceived signal " << signum << ", shutting down...\n";
    g_running = false;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        // Initialize curl globally
        if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
            throw std::runtime_error("curl_global_init failed");
        }

        // Load configuration
        auto env = load_env_file(".env");
        if (env.empty()) env = load_env_file("../.env");

        std::string api_key_id = get_env_value(env, "KALSHI_API_KEY_ID");
        std::string private_key_path = get_env_value(env, "KALSHI_PRIVATE_KEY");
        std::string kalshi_env = get_env_value(env, "KALSHI_ENV", "demo");
        bool is_prod = (kalshi_env == "prod");

        if (api_key_id.empty() || private_key_path.empty()) {
            throw std::runtime_error(
                "Missing KALSHI_API_KEY_ID or KALSHI_PRIVATE_KEY in .env file");
        }

        // Trading configuration
        trading::TradingConfig trading_config;
        trading_config.verbose = (get_env_value(env, "VERBOSE") == "true" ||
                                   get_env_value(env, "VERBOSE") == "1");
        trading_config.default_contracts = std::stoi(
            get_env_value(env, "DEFAULT_CONTRACTS", "5"));
        trading_config.default_price = std::stoi(
            get_env_value(env, "DEFAULT_PRICE", "99"));

        int poll_interval_sec = std::stoi(
            get_env_value(env, "POLL_INTERVAL_SEC", "2"));

        bool auto_discover = (get_env_value(env, "AUTO_DISCOVER", "true") == "true");

        // Print banner
        std::cout << "=================================================\n";
        std::cout << "  WEATHER TRADER v2 - Auto Discovery\n";
        std::cout << "=================================================\n\n";
        std::cout << "Environment:     " << (is_prod ? "PRODUCTION" : "DEMO") << "\n";
        std::cout << "Auto discover:   " << (auto_discover ? "ON" : "OFF") << "\n";
        std::cout << "Poll interval:   " << poll_interval_sec << " seconds\n";
        std::cout << "Default order:   " << trading_config.default_contracts
                  << " contracts @ " << trading_config.default_price << "c\n";
        std::cout << "Verbose:         " << (trading_config.verbose ? "ON" : "OFF") << "\n";
        std::cout << "\n";

        if (is_prod) {
            std::cout << "*** WARNING: PRODUCTION MODE - REAL MONEY AT RISK ***\n\n";
        }

        // Initialize Kalshi client
        kalshi::KalshiClient kalshi_client(api_key_id, private_key_path, is_prod);

        // Discover markets
        std::vector<CityMarkets> all_markets;

        if (auto_discover) {
            std::cout << "Discovering weather markets...\n";
            all_markets = kalshi_client.discover_weather_markets();

            if (all_markets.empty()) {
                std::cout << "No weather markets found. Exiting.\n";
                curl_global_cleanup();
                return 0;
            }

            trading::print_market_summary(all_markets);
        } else {
            std::cout << "Auto-discovery disabled. No markets to trade.\n";
            curl_global_cleanup();
            return 0;
        }

        // Collect unique stations from discovered markets
        std::set<std::string> unique_stations;
        for (const auto& city : all_markets) {
            unique_stations.insert(city.station);
        }

        std::cout << "Monitoring stations: ";
        bool first = true;
        for (const auto& stn : unique_stations) {
            if (!first) std::cout << ", ";
            std::cout << stn;
            first = false;
        }
        std::cout << "\n\n";

        // Initialize MADIS client
        madis::MadisClient madis_client;
        std::unordered_map<std::string, madis::FetchState> madis_states;

        // Map station -> city markets for quick lookup
        std::unordered_map<std::string, std::vector<CityMarkets*>> station_to_markets;
        for (auto& city : all_markets) {
            station_to_markets[city.station].push_back(&city);
        }

        std::cout << "Starting polling loop...\n\n";

        // Main polling loop
        while (g_running) {
            std::string time_str = timestamp_now();

            // Fetch latest observations for all stations
            std::unordered_map<std::string, madis::MadisClient::PollResult> poll_results;
            for (const auto& station : unique_stations) {
                poll_results[station] = madis_client.fetch_latest(station, madis_states[station]);
            }

            // Verbose logging
            if (trading_config.verbose) {
                for (const auto& station : unique_stations) {
                    const auto& result = poll_results[station];
                    if (result.not_modified) {
                        std::cout << "[" << time_str << "] " << station
                                  << " poll: 304 not modified (" << result.filename << ")\n";
                    } else if (result.is_new_data && result.obs) {
                        std::cout << "[" << time_str << "] " << station
                                  << " poll: 200 NEW DATA (" << result.filename << ")\n";
                    } else if (result.http_status > 0) {
                        std::cout << "[" << time_str << "] " << station
                                  << " poll: " << result.http_status
                                  << " (" << result.filename << ")\n";
                    }
                }
            }

            // Print temperature updates when new data arrives
            for (const auto& station : unique_stations) {
                const auto& result = poll_results[station];
                if (result.is_new_data && result.obs && result.obs->valid) {
                    int temp_f = c_to_f(result.obs->temp_c);
                    std::cout << "[" << time_str << "] " << station
                              << " temp: " << result.obs->temp_c << "°C / " << temp_f << "°F"
                              << " @ " << format_utc_time(result.obs->obs_time)
                              << " (file: " << result.obs->filename << ")\n";
                }
            }

            // Check each station's cached observation against its markets
            for (const auto& station : unique_stations) {
                const auto& cached = madis_states[station].cached_obs;
                if (!cached || !cached->valid) continue;

                // Check data staleness
                auto age = std::time(nullptr) - cached->obs_time;
                if (age > trading_config.stale_data_threshold_sec) {
                    if (trading_config.verbose) {
                        std::cout << "[" << time_str << "] " << station
                                  << " data too stale (" << age << "s old), skipping\n";
                    }
                    continue;
                }

                // Process all city markets for this station
                auto it = station_to_markets.find(station);
                if (it == station_to_markets.end()) continue;

                for (auto* city : it->second) {
                    trading::check_and_trade(*city, cached->temp_c, kalshi_client, trading_config);
                }
            }

            // Check if all buckets have been traded
            bool all_traded = true;
            for (const auto& city : all_markets) {
                for (const auto& bucket : city.low_buckets) {
                    if (!bucket.traded) { all_traded = false; break; }
                }
                if (!all_traded) break;
                for (const auto& bucket : city.high_buckets) {
                    if (!bucket.traded) { all_traded = false; break; }
                }
                if (!all_traded) break;
            }

            if (all_traded) {
                std::cout << "\n*** All buckets traded. Monitoring complete. ***\n";
                // Keep running to show temp updates, but no more orders will be placed
            }

            // Sleep before next poll
            std::this_thread::sleep_for(std::chrono::seconds(poll_interval_sec));
        }

        std::cout << "Shutdown complete.\n";
        curl_global_cleanup();
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "FATAL ERROR: " << e.what() << "\n";
        curl_global_cleanup();
        return 1;
    }
}
