// kalshi_client.hpp
// Kalshi REST API client with market discovery

#pragma once

#include "common.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/err.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace kalshi {

// ============================================================================
// RSA-PSS SIGNER
// ============================================================================

class RsaSigner {
public:
    explicit RsaSigner(const std::string& pem_path) {
        std::ifstream file(pem_path);
        if (!file) throw std::runtime_error("Cannot open private key: " + pem_path);

        std::stringstream ss;
        ss << file.rdbuf();
        std::string pem_data = ss.str();

        BIO* bio = BIO_new_mem_buf(pem_data.data(), static_cast<int>(pem_data.size()));
        if (!bio) throw std::runtime_error("BIO_new_mem_buf failed");

        pkey_ = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);

        if (!pkey_) throw std::runtime_error("Failed to load private key");
    }

    ~RsaSigner() { if (pkey_) EVP_PKEY_free(pkey_); }

    std::string sign(const std::string& message) const {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_PKEY_CTX* pkey_ctx = nullptr;

        if (EVP_DigestSignInit(ctx, &pkey_ctx, EVP_sha256(), nullptr, pkey_) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestSignInit failed");
        }

        EVP_PKEY_CTX_set_rsa_padding(pkey_ctx, RSA_PKCS1_PSS_PADDING);
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pkey_ctx, RSA_PSS_SALTLEN_DIGEST);

        EVP_DigestSignUpdate(ctx, message.data(), message.size());

        size_t sig_len = 0;
        EVP_DigestSignFinal(ctx, nullptr, &sig_len);

        std::vector<unsigned char> sig(sig_len);
        EVP_DigestSignFinal(ctx, sig.data(), &sig_len);
        EVP_MD_CTX_free(ctx);

        // Base64 encode
        BIO* bio = BIO_new(BIO_s_mem());
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        bio = BIO_push(b64, bio);
        BIO_write(bio, sig.data(), static_cast<int>(sig_len));
        BIO_flush(bio);

        BUF_MEM* buf;
        BIO_get_mem_ptr(bio, &buf);
        std::string result(buf->data, buf->length);
        BIO_free_all(bio);

        return result;
    }

private:
    EVP_PKEY* pkey_ = nullptr;
};

// ============================================================================
// CURL HELPERS
// ============================================================================

static size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// ============================================================================
// ORDER RESULT
// ============================================================================

enum class Side { YES, NO };

struct OrderResult {
    bool success = false;
    long status_code = 0;
    std::string order_id;
    std::string error;
    std::string raw_response;
};

// ============================================================================
// KALSHI CLIENT
// ============================================================================

class KalshiClient {
public:
    KalshiClient(const std::string& api_key_id, const std::string& private_key_path, bool is_prod)
        : api_key_id_(api_key_id), signer_(private_key_path), is_prod_(is_prod) {

        base_url_ = is_prod
            ? "https://api.elections.kalshi.com/trade-api/v2"
            : "https://demo-api.kalshi.co/trade-api/v2";

        curl_ = curl_easy_init();
        if (!curl_) throw std::runtime_error("curl_easy_init failed");
    }

    ~KalshiClient() { if (curl_) curl_easy_cleanup(curl_); }

    // Get base URL for WebSocket connection
    std::string get_ws_url() const {
        return is_prod_
            ? "wss://api.elections.kalshi.com/trade-api/ws/v2"
            : "wss://demo-api.kalshi.co/trade-api/ws/v2";
    }

    // ========================================================================
    // MARKET DISCOVERY
    // ========================================================================

    std::vector<CityMarkets> discover_weather_markets() {
        std::vector<CityMarkets> result;
        std::unordered_map<std::string, CityMarkets> city_map;

        // Fetch all open markets
        std::string cursor;
        int total_markets = 0;

        do {
            std::string url = base_url_ + "/markets?status=open&limit=200";
            if (!cursor.empty()) {
                url += "&cursor=" + cursor;
            }

            auto response = get_request(url);
            if (response.status_code != 200) {
                std::cerr << "Failed to fetch markets: " << response.status_code << "\n";
                break;
            }

            // Parse markets from response
            cursor = parse_markets_response(response.body, city_map);
            total_markets += count_weather_markets(response.body);

        } while (!cursor.empty());

        // Convert map to vector and sort buckets
        for (auto& [city_code, markets] : city_map) {
            // Sort low buckets by temperature (ascending)
            std::sort(markets.low_buckets.begin(), markets.low_buckets.end(),
                [](const BucketInfo& a, const BucketInfo& b) {
                    return a.low_f < b.low_f;
                });

            // Sort high buckets by temperature (ascending)
            std::sort(markets.high_buckets.begin(), markets.high_buckets.end(),
                [](const BucketInfo& a, const BucketInfo& b) {
                    return a.low_f < b.low_f;
                });

            result.push_back(std::move(markets));
        }

        return result;
    }

    // ========================================================================
    // ORDER PLACEMENT
    // ========================================================================

    OrderResult buy(const std::string& ticker, int count, Side side, int price_cents) {
        // Build order JSON
        std::string client_order_id = "weather_" + std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count());

        std::string side_str = (side == Side::YES) ? "yes" : "no";
        std::string price_field = (side == Side::YES) ? "yes_price" : "no_price";

        std::ostringstream json;
        json << R"({"ticker":")" << ticker << R"(",)"
             << R"("action":"buy",)"
             << R"("side":")" << side_str << R"(",)"
             << R"("count":)" << count << ","
             << R"("type":"limit",)"
             << R"(")" << price_field << R"(":)" << price_cents << ","
             << R"("client_order_id":")" << client_order_id << R"("})";

        return post_order(json.str());
    }

    // Get signing key and timestamp for WebSocket auth
    struct AuthInfo {
        std::string timestamp;
        std::string signature;
    };

    AuthInfo get_ws_auth() {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        std::string timestamp = std::to_string(ms);

        // For WS auth, sign: timestamp + "GET" + path
        std::string message = timestamp + "GET" + "/trade-api/ws/v2";
        std::string signature = signer_.sign(message);

        return {timestamp, signature};
    }

    const std::string& api_key() const { return api_key_id_; }

private:
    std::string api_key_id_;
    std::string base_url_;
    RsaSigner signer_;
    CURL* curl_;
    bool is_prod_;

    struct HttpResponse {
        long status_code = 0;
        std::string body;
    };

    HttpResponse get_request(const std::string& url) {
        HttpResponse response;

        curl_easy_reset(curl_);

        // Timestamp
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        std::string timestamp = std::to_string(ms);

        // Extract path from URL for signing
        std::string path;
        auto pos = url.find("/trade-api/v2");
        if (pos != std::string::npos) {
            path = url.substr(pos);
        }

        // Sign: timestamp + method + path
        std::string message = timestamp + "GET" + path;
        std::string signature = signer_.sign(message);

        // Headers
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, ("KALSHI-ACCESS-KEY: " + api_key_id_).c_str());
        headers = curl_slist_append(headers, ("KALSHI-ACCESS-TIMESTAMP: " + timestamp).c_str());
        headers = curl_slist_append(headers, ("KALSHI-ACCESS-SIGNATURE: " + signature).c_str());

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);

        CURLcode rc = curl_easy_perform(curl_);
        curl_slist_free_all(headers);

        if (rc == CURLE_OK) {
            curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &response.status_code);
        }

        return response;
    }

    OrderResult post_order(const std::string& json_body) {
        OrderResult result;

        curl_easy_reset(curl_);

        std::string path = "/portfolio/orders";
        std::string url = base_url_ + path;

        // Timestamp
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        std::string timestamp = std::to_string(ms);

        // Sign: timestamp + method + full_path
        std::string sign_path = "/trade-api/v2" + path;
        std::string message = timestamp + "POST" + sign_path;
        std::string signature = signer_.sign(message);

        // Headers
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, ("KALSHI-ACCESS-KEY: " + api_key_id_).c_str());
        headers = curl_slist_append(headers, ("KALSHI-ACCESS-TIMESTAMP: " + timestamp).c_str());
        headers = curl_slist_append(headers, ("KALSHI-ACCESS-SIGNATURE: " + signature).c_str());

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, json_body.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &result.raw_response);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);

        CURLcode rc = curl_easy_perform(curl_);
        curl_slist_free_all(headers);

        if (rc != CURLE_OK) {
            result.error = curl_easy_strerror(rc);
            return result;
        }

        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &result.status_code);
        result.success = (result.status_code == 200 || result.status_code == 201);

        // Extract order_id from response
        auto pos = result.raw_response.find("\"order_id\":\"");
        if (pos != std::string::npos) {
            pos += 12;
            auto end = result.raw_response.find("\"", pos);
            if (end != std::string::npos) {
                result.order_id = result.raw_response.substr(pos, end - pos);
            }
        }

        return result;
    }

    // Parse markets from JSON response and populate city_map
    // Returns cursor for pagination (empty if no more pages)
    std::string parse_markets_response(const std::string& json,
                                        std::unordered_map<std::string, CityMarkets>& city_map) {
        // Find cursor for pagination
        std::string cursor;
        auto cursor_pos = json.find("\"cursor\":\"");
        if (cursor_pos != std::string::npos) {
            cursor_pos += 10;
            auto end = json.find("\"", cursor_pos);
            if (end != std::string::npos) {
                cursor = json.substr(cursor_pos, end - cursor_pos);
            }
        }

        // Parse each market in the "markets" array
        // Look for tickers starting with KXLOWT or KXHIGH
        size_t pos = 0;
        while ((pos = json.find("\"ticker\":\"", pos)) != std::string::npos) {
            pos += 10;
            auto end = json.find("\"", pos);
            if (end == std::string::npos) break;

            std::string ticker = json.substr(pos, end - pos);
            pos = end;

            // Check if this is a weather market
            if (ticker.substr(0, 6) != "KXLOWT" && ticker.substr(0, 6) != "KXHIGH") {
                continue;
            }

            // Parse market details
            BucketInfo bucket;
            bucket.ticker = ticker;

            // Determine market type
            if (ticker.substr(0, 6) == "KXLOWT") {
                bucket.type = MarketType::LOW_TEMP;
            } else {
                bucket.type = MarketType::HIGH_TEMP;
            }

            // Extract city code (3 chars after KXLOWT or KXHIGH)
            std::string city_code;
            if (bucket.type == MarketType::LOW_TEMP && ticker.size() > 9) {
                city_code = ticker.substr(6, 3);
            } else if (bucket.type == MarketType::HIGH_TEMP && ticker.size() > 9) {
                city_code = ticker.substr(6, 3);
            }

            if (city_code.empty()) continue;

            // Parse bucket temperature from ticker
            // Format: KXLOWTCHI-26JAN15-B52 or KXHIGHCHI-26JAN15-T72
            // B = bucket lower bound, T = threshold
            std::regex bucket_regex(R"(([BT])(\d+(?:\.\d+)?))");
            std::smatch match;
            if (std::regex_search(ticker, match, bucket_regex)) {
                double temp = std::stod(match[2].str());
                bucket.low_f = static_cast<int>(temp);
                bucket.high_f = bucket.low_f + 1;  // Buckets are typically 1°F wide
                bucket.low_c = f_to_c(bucket.low_f);
                bucket.high_c = f_to_c(bucket.high_f);
            } else {
                continue;  // Skip if can't parse temperature
            }

            // Extract date suffix (e.g., 26JAN15)
            std::regex date_regex(R"((\d{2}[A-Z]{3}\d{2}))");
            std::string date_suffix;
            if (std::regex_search(ticker, match, date_regex)) {
                date_suffix = match[1].str();
            }

            // Look for market_id nearby in JSON
            auto id_pos = json.rfind("\"id\":\"", pos);
            if (id_pos != std::string::npos && id_pos > pos - 500) {
                id_pos += 6;
                auto id_end = json.find("\"", id_pos);
                if (id_end != std::string::npos) {
                    bucket.market_id = json.substr(id_pos, id_end - id_pos);
                }
            }

            // Add to city map
            auto station_it = CITY_TO_STATION.find(city_code);
            if (station_it == CITY_TO_STATION.end()) {
                continue;  // Unknown city
            }

            if (city_map.find(city_code) == city_map.end()) {
                city_map[city_code] = CityMarkets{
                    city_code,
                    station_it->second,
                    date_suffix,
                    {},
                    {}
                };
            }

            auto& city = city_map[city_code];
            if (!date_suffix.empty()) {
                city.date_suffix = date_suffix;
            }

            if (bucket.type == MarketType::LOW_TEMP) {
                city.low_buckets.push_back(bucket);
            } else {
                city.high_buckets.push_back(bucket);
            }
        }

        return cursor;
    }

    int count_weather_markets(const std::string& json) {
        int count = 0;
        size_t pos = 0;
        while ((pos = json.find("KXLOWT", pos)) != std::string::npos) {
            count++;
            pos++;
        }
        pos = 0;
        while ((pos = json.find("KXHIGH", pos)) != std::string::npos) {
            count++;
            pos++;
        }
        return count;
    }
};

} // namespace kalshi
