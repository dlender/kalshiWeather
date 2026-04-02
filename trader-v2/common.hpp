// common.hpp
// Shared types and utilities for weather trading system

#pragma once

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// CITY <-> STATION MAPPING
// ============================================================================

// Maps Kalshi city codes to MADIS weather station identifiers
const std::unordered_map<std::string, std::string> CITY_TO_STATION = {
    {"CHI", "KMDW"},   // Chicago Midway
    {"MIA", "KMIA"},   // Miami International
    {"DEN", "KDEN"},   // Denver International
    {"NYC", "KJFK"},   // New York JFK
    {"ATL", "KATL"},   // Atlanta Hartsfield
    {"LAX", "KLAX"},   // Los Angeles
    {"PHX", "KPHX"},   // Phoenix
    {"DFW", "KDFW"},   // Dallas/Fort Worth
    {"SEA", "KSEA"},   // Seattle
    {"BOS", "KBOS"},   // Boston
    {"MSP", "KMSP"},   // Minneapolis
    {"DET", "KDTW"},   // Detroit Metro
    {"SLC", "KSLC"},   // Salt Lake City
    {"LAS", "KLAS"},   // Las Vegas
    {"AUS", "KAUS"},   // Austin
};

// Reverse mapping: station -> city
inline std::unordered_map<std::string, std::string> build_station_to_city() {
    std::unordered_map<std::string, std::string> result;
    for (const auto& [city, station] : CITY_TO_STATION) {
        result[station] = city;
    }
    return result;
}

const std::unordered_map<std::string, std::string> STATION_TO_CITY = build_station_to_city();

// ============================================================================
// TEMPERATURE CONVERSION
// ============================================================================

// Celsius to Fahrenheit (standard formula)
inline int c_to_f(int celsius) {
    return (celsius * 9 / 5) + 32;
}

// Fahrenheit to Celsius
inline int f_to_c(int fahrenheit) {
    return (fahrenheit - 32) * 5 / 9;
}

// ============================================================================
// MARKET TYPES
// ============================================================================

enum class MarketType {
    LOW_TEMP,   // KXLOWT* - Low temperature markets
    HIGH_TEMP   // KXHIGH* - High temperature markets
};

// Information about a single temperature bucket
struct BucketInfo {
    std::string ticker;        // Full ticker e.g., "KXLOWTCHI-26JAN15-B52"
    std::string market_id;     // Kalshi market ID (UUID)
    MarketType type;           // LOW_TEMP or HIGH_TEMP
    int low_f;                 // Lower bound in Fahrenheit
    int high_f;                // Upper bound in Fahrenheit (usually low_f + 1)
    int low_c;                 // Lower bound converted to Celsius
    int high_c;                // Upper bound converted to Celsius
    bool traded = false;       // Already placed order for this bucket?

    // Liquidity info (updated by WebSocket)
    bool has_liquidity_data = false;
    int best_yes_bid = -1;
    int best_no_ask = -1;
    int total_liquidity = 0;

    bool has_liquidity() const {
        return has_liquidity_data && total_liquidity > 0;
    }
};

// All markets for a single city
struct CityMarkets {
    std::string city_code;     // e.g., "CHI"
    std::string station;       // e.g., "KMDW"
    std::string date_suffix;   // e.g., "26JAN15"
    std::vector<BucketInfo> low_buckets;   // Low temperature buckets (sorted by temp)
    std::vector<BucketInfo> high_buckets;  // High temperature buckets (sorted by temp)
};

// ============================================================================
// OBSERVATION EVENT (NATS message format)
// ============================================================================

struct ObservationEvent {
    std::string event_type;    // "metar_observation"
    std::string source;        // "madis_hfmetar"
    std::string station;       // "KMDW"
    std::time_t obs_time_utc;  // Observation timestamp
    std::time_t recv_time_utc; // Receipt timestamp
    std::string raw_metar;     // Full METAR string
    int temp_c = -999;         // Temperature in Celsius
    int dewpoint_c = -999;     // Dewpoint in Celsius
    std::string dedupe_key;    // Hash for deduplication

    bool valid() const {
        return temp_c != -999 && obs_time_utc > 0;
    }

    // Serialize to JSON for NATS
    std::string to_json() const {
        std::ostringstream oss;
        oss << R"({"event_type":")" << event_type << R"(",)"
            << R"("source":")" << source << R"(",)"
            << R"("station":")" << station << R"(",)"
            << R"("obs_time_utc":)" << obs_time_utc << ","
            << R"("recv_time_utc":)" << recv_time_utc << ","
            << R"("temp_c":)" << temp_c << ","
            << R"("dewpoint_c":)" << dewpoint_c << ","
            << R"("dedupe_key":")" << dedupe_key << R"(",)"
            << R"("raw_metar":")" << escape_json(raw_metar) << R"("})";
        return oss.str();
    }

    // Deserialize from JSON
    static std::optional<ObservationEvent> from_json(const std::string& json) {
        ObservationEvent evt;

        // Simple JSON parsing (no external library)
        auto get_string = [&](const std::string& key) -> std::string {
            std::string needle = "\"" + key + "\":\"";
            auto pos = json.find(needle);
            if (pos == std::string::npos) return "";
            pos += needle.size();
            auto end = json.find("\"", pos);
            if (end == std::string::npos) return "";
            return json.substr(pos, end - pos);
        };

        auto get_int = [&](const std::string& key) -> int {
            std::string needle = "\"" + key + "\":";
            auto pos = json.find(needle);
            if (pos == std::string::npos) return -999;
            pos += needle.size();
            // Skip whitespace
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
            // Parse number
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
        };

        evt.event_type = get_string("event_type");
        evt.source = get_string("source");
        evt.station = get_string("station");
        evt.obs_time_utc = static_cast<std::time_t>(get_int("obs_time_utc"));
        evt.recv_time_utc = static_cast<std::time_t>(get_int("recv_time_utc"));
        evt.temp_c = get_int("temp_c");
        evt.dewpoint_c = get_int("dewpoint_c");
        evt.dedupe_key = get_string("dedupe_key");
        evt.raw_metar = get_string("raw_metar");

        if (evt.station.empty()) return std::nullopt;
        return evt;
    }

private:
    static std::string escape_json(const std::string& s) {
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c; break;
            }
        }
        return result;
    }
};

// ============================================================================
// CONFIGURATION UTILITIES
// ============================================================================

inline std::unordered_map<std::string, std::string> load_env_file(const std::string& path) {
    std::unordered_map<std::string, std::string> env;
    std::ifstream file(path);
    if (!file) return env;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        // Trim whitespace
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
               value.back() == '\r' || value.back() == '\n')) value.pop_back();

        // Remove quotes
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        if (!key.empty()) env[key] = value;
    }
    return env;
}

inline std::string get_env_value(const std::unordered_map<std::string, std::string>& env,
                                  const std::string& key,
                                  const std::string& default_val = "") {
    auto it = env.find(key);
    if (it != env.end() && !it->second.empty()) return it->second;
    const char* val = std::getenv(key.c_str());
    return val ? std::string(val) : default_val;
}

// ============================================================================
// LOGGING UTILITIES
// ============================================================================

inline std::string timestamp_now() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time_t_now, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}

inline std::string format_utc_time(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    return std::string(buf);
}
