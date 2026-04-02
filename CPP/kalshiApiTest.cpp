// kalshiApiTest.cpp
//
// Test client for Kalshi REST API - market data and order execution
// Uses RSA-PSS signing for authentication
//
// WSL/Ubuntu setup:
//   sudo apt update
//   sudo apt install -y build-essential libcurl4-openssl-dev libssl-dev nlohmann-json3-dev
//
// Build:
//   g++ -O2 -std=c++20 kalshiApiTest.cpp -lcurl -lssl -lcrypto -o kalshiApiTest
//
// Run:
//   ./kalshiApiTest
//
// Configuration (via .env file or environment variables):
//   KALSHI_API_KEY_ID    - Your API key ID from Kalshi
//   KALSHI_PRIVATE_KEY   - Path to your RSA private key PEM file
//   KALSHI_ENV           - "demo" or "prod" (default: demo)
//
// Create a .env file in the same directory as the executable:
//   KALSHI_API_KEY_ID=your-key-id
//   KALSHI_PRIVATE_KEY=C:/path/to/kalshi.pem
//   KALSHI_ENV=demo

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/err.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kalshi {

// ============================== .env File Parser ==============================

// Simple .env file parser - loads KEY=VALUE pairs into a map
std::unordered_map<std::string, std::string> load_env_file(const std::string& path) {
    std::unordered_map<std::string, std::string> env;
    std::ifstream file(path);

    if (!file) {
        return env;  // File doesn't exist, return empty map
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        // Find the = separator
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        // Trim whitespace from key
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(0, 1);

        // Trim whitespace and quotes from value
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
               value.back() == '\r' || value.back() == '\n')) value.pop_back();
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);

        // Remove surrounding quotes if present
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        if (!key.empty()) {
            env[key] = value;
        }
    }

    return env;
}

// Get config value: check env file first, then environment variable
std::string get_config_value(const std::unordered_map<std::string, std::string>& env_file,
                             const std::string& key) {
    // Check .env file first
    auto it = env_file.find(key);
    if (it != env_file.end() && !it->second.empty()) {
        return it->second;
    }

    // Fall back to environment variable
    const char* val = std::getenv(key.c_str());
    return val ? std::string(val) : "";
}

// ============================== Configuration ==============================

struct Config {
    std::string api_key_id;
    std::string private_key_path;
    std::string base_url;
    bool is_demo = true;

    static Config from_env(const std::string& env_file_path = ".env") {
        Config cfg;

        // Try to load .env file from current directory and parent directory
        auto env = load_env_file(env_file_path);
        if (env.empty()) {
            env = load_env_file("../.env");
        }
        if (env.empty()) {
            env = load_env_file("../kalshi.env");
        }

        std::string key_id = get_config_value(env, "KALSHI_API_KEY_ID");
        std::string key_path = get_config_value(env, "KALSHI_PRIVATE_KEY");
        std::string environment = get_config_value(env, "KALSHI_ENV");

        if (key_id.empty() || key_path.empty()) {
            throw std::runtime_error(
                "Missing configuration. Create a .env file with:\n"
                "  KALSHI_API_KEY_ID=your-key-id\n"
                "  KALSHI_PRIVATE_KEY=C:/path/to/kalshi.pem\n"
                "  KALSHI_ENV=demo\n\n"
                "Or set environment variables:\n"
                "  KALSHI_API_KEY_ID    - Your API key ID\n"
                "  KALSHI_PRIVATE_KEY   - Path to RSA private key PEM file\n"
                "  KALSHI_ENV           - 'demo' or 'prod' (optional, default: demo)"
            );
        }

        cfg.api_key_id = key_id;
        cfg.private_key_path = key_path;

        if (environment == "prod") {
            cfg.base_url = "https://api.elections.kalshi.com/trade-api/v2";
            cfg.is_demo = false;
        } else {
            cfg.base_url = "https://demo-api.kalshi.co/trade-api/v2";
            cfg.is_demo = true;
        }

        return cfg;
    }
};

// ============================== RSA-PSS Signing ==============================

class RsaSigner {
public:
    explicit RsaSigner(const std::string& pem_path) {
        std::ifstream file(pem_path);
        if (!file) {
            throw std::runtime_error("Cannot open private key file: " + pem_path);
        }

        std::stringstream ss;
        ss << file.rdbuf();
        std::string pem_data = ss.str();

        BIO* bio = BIO_new_mem_buf(pem_data.data(), static_cast<int>(pem_data.size()));
        if (!bio) {
            throw std::runtime_error("BIO_new_mem_buf failed");
        }

        pkey_ = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);

        if (!pkey_) {
            unsigned long err = ERR_get_error();
            char buf[256];
            ERR_error_string_n(err, buf, sizeof(buf));
            throw std::runtime_error(std::string("Failed to load private key: ") + buf);
        }
    }

    ~RsaSigner() {
        if (pkey_) {
            EVP_PKEY_free(pkey_);
        }
    }

    RsaSigner(const RsaSigner&) = delete;
    RsaSigner& operator=(const RsaSigner&) = delete;

    // Sign message with RSA-PSS (SHA256) and return base64-encoded signature
    std::string sign(const std::string& message) const {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) {
            throw std::runtime_error("EVP_MD_CTX_new failed");
        }

        // Initialize signing with RSA-PSS padding
        EVP_PKEY_CTX* pkey_ctx = nullptr;
        if (EVP_DigestSignInit(ctx, &pkey_ctx, EVP_sha256(), nullptr, pkey_) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestSignInit failed");
        }

        // Set PSS padding with salt length = digest length (32 bytes for SHA256)
        if (EVP_PKEY_CTX_set_rsa_padding(pkey_ctx, RSA_PKCS1_PSS_PADDING) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_PKEY_CTX_set_rsa_padding failed");
        }

        if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pkey_ctx, RSA_PSS_SALTLEN_DIGEST) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_PKEY_CTX_set_rsa_pss_saltlen failed");
        }

        // Update with message
        if (EVP_DigestSignUpdate(ctx, message.data(), message.size()) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestSignUpdate failed");
        }

        // Get signature length
        size_t sig_len = 0;
        if (EVP_DigestSignFinal(ctx, nullptr, &sig_len) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestSignFinal (get length) failed");
        }

        // Generate signature
        std::vector<unsigned char> sig(sig_len);
        if (EVP_DigestSignFinal(ctx, sig.data(), &sig_len) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestSignFinal failed");
        }

        EVP_MD_CTX_free(ctx);

        // Base64 encode
        return base64_encode(sig.data(), sig_len);
    }

private:
    EVP_PKEY* pkey_ = nullptr;

    static std::string base64_encode(const unsigned char* data, size_t len) {
        BIO* bio = BIO_new(BIO_s_mem());
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        bio = BIO_push(b64, bio);

        BIO_write(bio, data, static_cast<int>(len));
        BIO_flush(bio);

        BUF_MEM* buf = nullptr;
        BIO_get_mem_ptr(bio, &buf);

        std::string result(buf->data, buf->length);
        BIO_free_all(bio);

        return result;
    }
};

// ============================== HTTP Client ==============================

static size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

class CurlGlobal {
public:
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
            throw std::runtime_error("curl_global_init failed");
        }
    }
    ~CurlGlobal() {
        curl_global_cleanup();
    }
};

struct HttpResponse {
    long status_code = 0;
    std::string body;
};

class KalshiClient {
public:
    KalshiClient(const Config& config)
        : config_(config), signer_(config.private_key_path) {
        curl_ = curl_easy_init();
        if (!curl_) {
            throw std::runtime_error("curl_easy_init failed");
        }
    }

    ~KalshiClient() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
    }

    KalshiClient(const KalshiClient&) = delete;
    KalshiClient& operator=(const KalshiClient&) = delete;

    // GET request
    HttpResponse get(const std::string& path) {
        return request("GET", path, "");
    }

    // POST request with JSON body
    HttpResponse post(const std::string& path, const std::string& json_body) {
        return request("POST", path, json_body);
    }

    // DELETE request
    HttpResponse del(const std::string& path) {
        return request("DELETE", path, "");
    }

private:
    Config config_;
    RsaSigner signer_;
    CURL* curl_ = nullptr;

    // Get current timestamp in milliseconds
    static std::string get_timestamp_ms() {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
        return std::to_string(ms);
    }

    // Strip query parameters from path for signing
    static std::string strip_query(const std::string& path) {
        auto pos = path.find('?');
        if (pos != std::string::npos) {
            return path.substr(0, pos);
        }
        return path;
    }

    HttpResponse request(const std::string& method, const std::string& path,
                         const std::string& body) {
        curl_easy_reset(curl_);

        std::string url = config_.base_url + path;
        std::string timestamp = get_timestamp_ms();
        std::string sign_path = strip_query(path);

        // Create message to sign: timestamp + method + FULL path (including /trade-api/v2)
        // Kalshi requires the full path in the signature
        std::string full_sign_path = "/trade-api/v2" + sign_path;
        std::string message = timestamp + method + full_sign_path;
        std::string signature = signer_.sign(message);

        // Set URL
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());

        // Set method
        if (method == "POST") {
            curl_easy_setopt(curl_, CURLOPT_POST, 1L);
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        } else if (method == "DELETE") {
            curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
        } else {
            curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
        }

        // Build headers
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers,
            ("KALSHI-ACCESS-KEY: " + config_.api_key_id).c_str());
        headers = curl_slist_append(headers,
            ("KALSHI-ACCESS-TIMESTAMP: " + timestamp).c_str());
        headers = curl_slist_append(headers,
            ("KALSHI-ACCESS-SIGNATURE: " + signature).c_str());

        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

        // Response handling
        HttpResponse response;
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response.body);

        // Timeouts
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);

        // Execute
        CURLcode rc = curl_easy_perform(curl_);
        curl_slist_free_all(headers);

        if (rc != CURLE_OK) {
            throw std::runtime_error(std::string("curl request failed: ") +
                                    curl_easy_strerror(rc));
        }

        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &response.status_code);
        return response;
    }
};

// ============================== Test Functions ==============================

void print_separator(const std::string& title) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(60, '=') << "\n\n";
}

void test_get_markets(KalshiClient& client) {
    print_separator("TEST: Get Markets");

    // Get a few markets to find one we can use for testing
    auto resp = client.get("/markets?limit=5&status=open");

    std::cout << "Status: " << resp.status_code << "\n";
    std::cout << "Response:\n" << resp.body << "\n";
}

void test_get_market(KalshiClient& client, const std::string& ticker) {
    print_separator("TEST: Get Market Details for " + ticker);

    auto resp = client.get("/markets/" + ticker);

    std::cout << "Status: " << resp.status_code << "\n";
    std::cout << "Response:\n" << resp.body << "\n";
}

void test_search_markets(KalshiClient& client, const std::string& series_ticker) {
    print_separator("TEST: Search Markets in Series " + series_ticker);

    // Get markets for a specific series (e.g., KXLOWTCHI for Chicago low temp)
    auto resp = client.get("/markets?series_ticker=" + series_ticker + "&status=open&limit=10");

    std::cout << "Status: " << resp.status_code << "\n";
    std::cout << "Response:\n" << resp.body << "\n";
}

void test_get_orderbook(KalshiClient& client, const std::string& ticker) {
    print_separator("TEST: Get Orderbook for " + ticker);

    auto resp = client.get("/markets/" + ticker + "/orderbook?depth=5");

    std::cout << "Status: " << resp.status_code << "\n";
    std::cout << "Response:\n" << resp.body << "\n";
}

void test_get_balance(KalshiClient& client) {
    print_separator("TEST: Get Portfolio Balance");

    auto resp = client.get("/portfolio/balance");

    std::cout << "Status: " << resp.status_code << "\n";
    std::cout << "Response:\n" << resp.body << "\n";
}

void test_get_positions(KalshiClient& client) {
    print_separator("TEST: Get Portfolio Positions");

    auto resp = client.get("/portfolio/positions");

    std::cout << "Status: " << resp.status_code << "\n";
    std::cout << "Response:\n" << resp.body << "\n";
}

void test_get_orders(KalshiClient& client) {
    print_separator("TEST: Get Open Orders");

    auto resp = client.get("/portfolio/orders?status=resting");

    std::cout << "Status: " << resp.status_code << "\n";
    std::cout << "Response:\n" << resp.body << "\n";
}

void test_create_order(KalshiClient& client, const std::string& ticker, bool dry_run = true) {
    print_separator("TEST: Create Order" + std::string(dry_run ? " (DRY RUN)" : ""));

    // Build order JSON manually (no external JSON library dependency)
    // This creates a limit order to buy 1 YES contract at 1 cent (very unlikely to fill)
    std::string order_json = R"({
        "ticker": ")" + ticker + R"(",
        "action": "sell",
        "side": "no",
        "count": 1,
        "type": "limit",
        "no_price": 85,
        "client_order_id": "test_)" + std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count()
        ) + R"("
    })";

    std::cout << "Order JSON:\n" << order_json << "\n\n";

    if (dry_run) {
        std::cout << "[DRY RUN] Order NOT submitted. Set dry_run=false to actually submit.\n";
        return;
    }

    auto resp = client.post("/portfolio/orders", order_json);

    std::cout << "Status: " << resp.status_code << "\n";
    std::cout << "Response:\n" << resp.body << "\n";
}

void test_cancel_order(KalshiClient& client, const std::string& order_id) {
    print_separator("TEST: Cancel Order " + order_id);

    auto resp = client.del("/portfolio/orders/" + order_id);

    std::cout << "Status: " << resp.status_code << "\n";
    std::cout << "Response:\n" << resp.body << "\n";
}

} // namespace kalshi

// ============================== Main ==============================

int main(int argc, char* argv[]) {
    try {
        kalshi::CurlGlobal curl_init;

        std::cout << "Kalshi API Test Client\n";
        std::cout << "======================\n\n";

        // Load config from environment
        auto config = kalshi::Config::from_env();

        std::cout << "Environment: " << (config.is_demo ? "DEMO" : "PRODUCTION") << "\n";
        std::cout << "Base URL: " << config.base_url << "\n";
        std::cout << "API Key ID: " << config.api_key_id << "\n\n";

        if (!config.is_demo) {
            std::cout << "*** WARNING: Running against PRODUCTION API ***\n";
            std::cout << "*** Real money is at risk! ***\n\n";
        }

        // Create client
        kalshi::KalshiClient client(config);

        // Run tests

        // 1. Search for Chicago low temp markets to find valid tickers
        // Try different case variations
        kalshi::test_search_markets(client, "KXLOWTCHI");

        // Also try searching by event ticker and with text search
        kalshi::print_separator("TEST: Search by event_ticker");
        auto resp1 = client.get("/markets?event_ticker=KXLOWTCHI&limit=10");
        std::cout << "Status: " << resp1.status_code << "\nResponse:\n" << resp1.body << "\n";

        kalshi::print_separator("TEST: Get Events for weather");
        auto resp2 = client.get("/events?series_ticker=KXLOWTCHI&limit=5");
        std::cout << "Status: " << resp2.status_code << "\nResponse:\n" << resp2.body << "\n";

        // 2. Test with a specific ticker (use a common one or pass via command line)
        // Full ticker format: SERIES-DATE-BRACKET (e.g., KXLOWTCHI-26JAN14-B19.5)
        std::string test_ticker = "KXLOWTCHI-26JAN14-B19.5";
        if (argc > 1) {
            test_ticker = argv[1];
        }

        // 3. Get details for the specific market
        kalshi::test_get_market(client, test_ticker);

        // 4. Get orderbook
        kalshi::test_get_orderbook(client, test_ticker);

        // 5. Test portfolio endpoints (authenticated)
        kalshi::test_get_balance(client);
        kalshi::test_get_positions(client);
        kalshi::test_get_orders(client);

        // 6. Test order creation (DRY RUN by default)
        // Pass --submit flag to actually submit the order
        bool submit_order = (argc > 2 && std::string(argv[2]) == "--submit");
        kalshi::test_create_order(client, test_ticker, !submit_order);

        std::cout << "\n" << std::string(60, '=') << "\n";
        std::cout << "  All tests completed!\n";
        std::cout << std::string(60, '=') << "\n\n";

        std::cout << "Usage:\n";
        std::cout << "  " << argv[0] << " [ticker] [--submit]\n\n";
        std::cout << "  ticker    - Market ticker to test with (default: KXINXD-26JAN10)\n";
        std::cout << "  --submit  - Actually submit a test order (otherwise dry run)\n";

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
