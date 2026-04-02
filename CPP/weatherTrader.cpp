// weatherTrader.cpp
//
// Automated weather trading system:
// - Polls MADIS METAR data every 2 seconds
// - Monitors Chicago (KMDW) temperature
// - If temp drops to -8°C or lower, buys NO contracts on KXLOWTCHI-26JAN14-B19.5
//
// WSL/Ubuntu setup:
//   sudo apt update
//   sudo apt install -y build-essential libcurl4-openssl-dev libssl-dev zlib1g-dev libnetcdf-dev
//
// Build:
//   g++ -O2 -std=c++20 weatherTrader.cpp -lcurl -lssl -lcrypto -lz -lnetcdf -o weatherTrader
//
// Run:
//   ./weatherTrader
//
// Configuration (via .env file):
//   KALSHI_API_KEY_ID    - Your API key ID from Kalshi
//   KALSHI_PRIVATE_KEY   - Path to your RSA private key PEM file
//   KALSHI_ENV           - "demo" or "prod" (default: demo)

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/err.h>
#include <zlib.h>
#include <netcdf.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// temp-file helpers (portable)
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

// ============================================================================
// CONFIGURATION
// ============================================================================

enum class ComparisonOp {
    LESS_EQUAL,    // <=
    GREATER_EQUAL  // >=
};

enum class Side {
    YES,
    NO
};

struct TradingStrategy {
    std::string name;                       // Strategy name for logging
    std::string market_ticker;              // Market ticker to trade
    std::string station;                    // Weather station code (e.g., "KMDW", "KMIA")
    ComparisonOp op;                        // Comparison operator
    int temp_threshold_c;                   // Temperature threshold
    int order_count;                        // Number of contracts
    Side side;                              // YES or NO
    int price_cents;                        // Limit price in cents
    bool order_placed;                      // Track if order already placed

    TradingStrategy(const std::string& n, const std::string& ticker, const std::string& stn,
                    ComparisonOp o, int threshold, int count, Side s, int price)
        : name(n), market_ticker(ticker), station(stn), op(o), temp_threshold_c(threshold),
          order_count(count), side(s), price_cents(price), order_placed(false) {}
};

struct TradingConfig {
    // MADIS settings
    int poll_interval_sec = 2;              // Poll every 2 seconds
    bool verbose = false;                   // Verbose logging (show every poll)

    // Trading strategies (each strategy specifies its own station)
    std::vector<TradingStrategy> strategies;
};

// ============================================================================
// .ENV FILE PARSER
// ============================================================================

std::unordered_map<std::string, std::string> load_env_file(const std::string& path) {
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

        // Trim
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

std::string get_env_value(const std::unordered_map<std::string, std::string>& env,
                          const std::string& key) {
    auto it = env.find(key);
    if (it != env.end() && !it->second.empty()) return it->second;
    const char* val = std::getenv(key.c_str());
    return val ? std::string(val) : "";
}

// ============================================================================
// MADIS INGESTOR (from madisIngestor.cpp)
// ============================================================================

namespace madis {

static constexpr std::string_view kBaseUrl =
    "https://madis-data.ncep.noaa.gov/madisPublic1/data/LDAD/hfmetar/netCDF";

struct HttpMeta {
    long status = 0;
    std::string etag;
    std::string last_modified;
};

static size_t write_to_vec(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(userdata);
    const size_t bytes = size * nmemb;
    out->insert(out->end(), static_cast<std::uint8_t*>(ptr),
                static_cast<std::uint8_t*>(ptr) + bytes);
    return bytes;
}

static size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* meta = static_cast<HttpMeta*>(userdata);
    std::string line(buffer, size * nitems);

    auto tolower_prefix = [&](const std::string& prefix) {
        if (line.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); i++) {
            char c = line[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != prefix[i]) return false;
        }
        return true;
    };

    auto trim = [](std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
            s.pop_back();
        size_t i = 0;
        while (i < s.size() && s[i] == ' ') i++;
        return s.substr(i);
    };

    if (tolower_prefix("etag:")) {
        meta->etag = trim(line.substr(5));
    } else if (tolower_prefix("last-modified:")) {
        meta->last_modified = trim(line.substr(14));
    }
    return size * nitems;
}

static std::vector<std::uint8_t> gunzip(const std::vector<std::uint8_t>& gz) {
    if (gz.size() < 2) throw std::runtime_error("gunzip: input too small");

    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {
        throw std::runtime_error("gunzip: inflateInit2 failed");
    }

    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(gz.data()));
    zs.avail_in = static_cast<uInt>(gz.size());

    std::vector<std::uint8_t> out;
    out.reserve(gz.size() * 3);
    std::uint8_t buf[32768];

    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        zs.next_out = buf;
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            throw std::runtime_error("gunzip failed");
        }
        out.insert(out.end(), buf, buf + (sizeof(buf) - zs.avail_out));
    }
    inflateEnd(&zs);
    return out;
}

// Observation data extracted from METAR
struct Observation {
    std::string station;
    std::time_t obs_time;
    int temp_c;           // Temperature in Celsius
    int dewpoint_c;       // Dewpoint in Celsius
    std::string raw_metar;
    std::string filename; // Name of the MADIS file used
    bool valid = false;
};

struct FetchState {
    std::string last_url;
    std::string last_etag;
    std::string last_modified;
    int last_hour = -1;  // Track which hour we last successfully fetched
    std::optional<Observation> cached_obs;  // Cached observation for 304 responses
};

class MadisClient {
public:
    MadisClient() {
        curl_ = curl_easy_init();
        if (!curl_) throw std::runtime_error("curl_easy_init failed");
    }

    ~MadisClient() {
        if (curl_) curl_easy_cleanup(curl_);
    }

    // Result of a fetch operation with verbose info
    struct PollResult {
        std::optional<Observation> obs;  // New observation (if any)
        bool is_new_data = false;        // True if this is newly downloaded data
        bool not_modified = false;       // True if 304 response
        int http_status = 0;
        std::string filename;
    };

    PollResult fetch_latest(const std::string& station, FetchState& state) {
        // Try current hour and previous hours
        auto now = std::time(nullptr);
        std::tm tm{};
        gmtime_r(&now, &tm);
        std::time_t floored = now - (tm.tm_min * 60) - tm.tm_sec;
        int current_hour = tm.tm_hour;

        // If hour changed, clear cached state to force fresh fetch
        if (state.last_hour != -1 && state.last_hour != current_hour) {
            state.last_url.clear();
            state.last_etag.clear();
            state.last_modified.clear();
            state.cached_obs = std::nullopt;
        }

        for (int i = 0; i < 3; ++i) {
            std::time_t tt = floored - (i * 3600);
            std::tm t{};
            gmtime_r(&tt, &t);

            char filename[64];
            snprintf(filename, sizeof(filename), "%04d%02d%02d_%02d00.gz",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);

            std::string url = std::string(kBaseUrl) + "/" + filename;

            auto result = try_fetch_url(url, station, state, filename);

            // If we got a new observation, cache it and return
            if (result.new_obs && result.new_obs->valid) {
                state.cached_obs = result.new_obs;
                state.last_hour = t.tm_hour;
                return {result.new_obs, true, false, result.http_status, result.filename};
            }

            // If 304 (not modified), return cached obs if we have one
            if (result.not_modified && state.cached_obs) {
                return {std::nullopt, false, true, 304, result.filename};
            }

            // If 404 or other error, try next (older) file
            // But if we have a cached obs, don't fall back to older files
            if (state.cached_obs) {
                return {std::nullopt, false, false, result.http_status, result.filename};
            }
        }
        return {std::nullopt, false, false, 0, ""};
    }

private:
    CURL* curl_;

    struct FetchResult {
        std::optional<Observation> new_obs;
        bool not_modified = false;
        int http_status = 0;
        std::string filename;
    };

    FetchResult try_fetch_url(const std::string& url,
                               const std::string& target_station,
                               FetchState& state,
                               const std::string& filename) {
        curl_easy_reset(curl_);

        std::vector<std::uint8_t> body;
        HttpMeta meta{};

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_to_vec);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &meta);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 5L);

        // Conditional GET
        struct curl_slist* headers = nullptr;
        if (!state.last_etag.empty() && url == state.last_url) {
            headers = curl_slist_append(headers, ("If-None-Match: " + state.last_etag).c_str());
        }
        if (headers) curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

        CURLcode rc = curl_easy_perform(curl_);
        if (headers) curl_slist_free_all(headers);

        if (rc != CURLE_OK) return {std::nullopt, false, 0, filename};

        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &meta.status);

        if (meta.status == 304) {
            return {std::nullopt, true, 304, filename};  // Not modified
        }

        if (meta.status != 200 || body.empty()) {
            return {std::nullopt, false, static_cast<int>(meta.status), filename};
        }

        // Update state
        state.last_url = url;
        state.last_etag = meta.etag;
        state.last_modified = meta.last_modified;

        // Decompress and parse
        try {
            auto netcdf = gunzip(body);
            auto obs = parse_netcdf(netcdf, target_station);
            if (obs && obs->valid) {
                obs->filename = filename;
            }
            return {obs, false, 200, filename};
        } catch (...) {
            return {std::nullopt, false, 200, filename};
        }
    }

    // Parse a specific field from rawMessage CSV string
    // Format: KMDW,1,26/01/15 04:45:00,0,0,1,4000,-999,-999,BKN, , ,10, ,0,-7,-14,...
    static std::string parse_field_from_raw(const std::string& raw, int target_field) {
        int field = 0;
        size_t start = 0;

        for (size_t i = 0; i <= raw.size(); i++) {
            if (i == raw.size() || raw[i] == ',') {
                if (field == target_field) {
                    std::string val = raw.substr(start, i - start);
                    while (!val.empty() && val.front() == ' ') val.erase(0, 1);
                    while (!val.empty() && val.back() == ' ') val.pop_back();
                    return val;
                }
                field++;
                start = i + 1;
            }
        }
        return "";
    }

    // Parse temperature from rawMessage (field 15)
    static int parse_temp_from_raw(const std::string& raw) {
        std::string val = parse_field_from_raw(raw, 15);
        try {
            return val.empty() ? -999 : std::stoi(val);
        } catch (...) {
            return -999;
        }
    }

    // Parse observation time from rawMessage (field 2)
    // Format: YY/MM/DD HH:MM:SS (e.g., "26/01/15 04:45:00")
    static std::time_t parse_time_from_raw(const std::string& raw) {
        std::string val = parse_field_from_raw(raw, 2);
        if (val.size() < 17) return 0;  // "YY/MM/DD HH:MM:SS" is 17 chars

        try {
            std::tm tm{};
            tm.tm_year = std::stoi(val.substr(0, 2)) + 100;  // YY -> years since 1900
            tm.tm_mon = std::stoi(val.substr(3, 2)) - 1;     // MM -> 0-based month
            tm.tm_mday = std::stoi(val.substr(6, 2));        // DD
            tm.tm_hour = std::stoi(val.substr(9, 2));        // HH
            tm.tm_min = std::stoi(val.substr(12, 2));        // MM
            tm.tm_sec = std::stoi(val.substr(15, 2));        // SS
            return timegm(&tm);  // Convert to UTC time_t
        } catch (...) {
            return 0;
        }
    }

    std::optional<Observation> parse_netcdf(const std::vector<std::uint8_t>& data,
                                             const std::string& target_station) {
        // Write to temp file (nc_open_mem not always available)
        char tmpl[] = "/tmp/madisXXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) return std::nullopt;

        write(fd, data.data(), data.size());
        close(fd);

        int ncid = -1;
        int rc = nc_open(tmpl, NC_NOWRITE, &ncid);
        unlink(tmpl);

        if (rc != NC_NOERR) return std::nullopt;

        Observation best_obs;
        best_obs.valid = false;
        double best_time = -1e300;

        try {
            // Get variable IDs
            int station_varid = -1, raw_varid = -1, time_varid = -1;

            auto try_var = [&](int& varid, std::initializer_list<const char*> names) {
                for (auto* name : names) {
                    if (nc_inq_varid(ncid, name, &varid) == NC_NOERR) return true;
                }
                return false;
            };

            if (!try_var(station_varid, {"stationId", "stationID", "station_id"})) {
                nc_close(ncid);
                return std::nullopt;
            }

            try_var(raw_varid, {"rawMessage", "raw_message"});
            try_var(time_varid, {"timeObs", "obsTime", "time_observation"});

            // Get station dimension
            int ndims;
            int dimids[NC_MAX_VAR_DIMS];
            nc_inq_var(ncid, station_varid, nullptr, nullptr, &ndims, dimids, nullptr);

            size_t nobs, station_len;
            nc_inq_dimlen(ncid, dimids[0], &nobs);
            nc_inq_dimlen(ncid, dimids[1], &station_len);

            // Read all stations
            std::vector<char> stations(nobs * station_len);
            nc_get_var_text(ncid, station_varid, stations.data());

            // Read times
            std::vector<double> times(nobs);
            if (time_varid >= 0) {
                nc_get_var_double(ncid, time_varid, times.data());
            }

            // Read raw messages
            size_t raw_len = 0;
            std::vector<char> raw_messages;
            if (raw_varid >= 0) {
                int raw_dimids[NC_MAX_VAR_DIMS];
                nc_inq_var(ncid, raw_varid, nullptr, nullptr, nullptr, raw_dimids, nullptr);
                nc_inq_dimlen(ncid, raw_dimids[1], &raw_len);
                raw_messages.resize(nobs * raw_len);
                nc_get_var_text(ncid, raw_varid, raw_messages.data());
            }

            // Find best observation for target station
            for (size_t i = 0; i < nobs; i++) {
                std::string station(&stations[i * station_len], station_len);
                // Trim trailing spaces/nulls
                while (!station.empty() && (station.back() == ' ' || station.back() == '\0'))
                    station.pop_back();

                if (station != target_station) continue;

                // Get observation time - prefer NetCDF time, fall back to raw message
                double obs_time = times[i];
                std::string raw_metar;
                if (!raw_messages.empty()) {
                    raw_metar = std::string(&raw_messages[i * raw_len], raw_len);
                    while (!raw_metar.empty() &&
                           (raw_metar.back() == ' ' || raw_metar.back() == '\0'))
                        raw_metar.pop_back();

                    // If NetCDF time is 0, parse from raw message for comparison
                    if (obs_time == 0) {
                        obs_time = static_cast<double>(parse_time_from_raw(raw_metar));
                    }
                }

                if (obs_time > best_time) {
                    best_time = obs_time;
                    best_obs.station = station;
                    best_obs.obs_time = static_cast<std::time_t>(obs_time);
                    best_obs.raw_metar = raw_metar;
                    best_obs.temp_c = parse_temp_from_raw(raw_metar);
                    best_obs.valid = true;
                }
            }

            nc_close(ncid);
            return best_obs.valid ? std::make_optional(best_obs) : std::nullopt;

        } catch (...) {
            nc_close(ncid);
            return std::nullopt;
        }
    }
};

} // namespace madis

// ============================================================================
// KALSHI CLIENT (from kalshiApiTest.cpp)
// ============================================================================

namespace kalshi {

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

static size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

struct OrderResult {
    bool success = false;
    long status_code = 0;
    std::string order_id;
    std::string error;
    std::string raw_response;
};

class KalshiClient {
public:
    KalshiClient(const std::string& api_key_id, const std::string& private_key_path, bool is_prod)
        : api_key_id_(api_key_id), signer_(private_key_path) {

        base_url_ = is_prod
            ? "https://api.elections.kalshi.com/trade-api/v2"
            : "https://demo-api.kalshi.co/trade-api/v2";

        curl_ = curl_easy_init();
        if (!curl_) throw std::runtime_error("curl_easy_init failed");
    }

    ~KalshiClient() { if (curl_) curl_easy_cleanup(curl_); }

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

private:
    std::string api_key_id_;
    std::string base_url_;
    RsaSigner signer_;
    CURL* curl_;

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

        // Extract order_id from response (simple parsing)
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
};

} // namespace kalshi

// ============================================================================
// MAIN TRADING LOOP
// ============================================================================

int main() {
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
        std::string kalshi_env = get_env_value(env, "KALSHI_ENV");
        bool is_prod = (kalshi_env == "prod");

        if (api_key_id.empty() || private_key_path.empty()) {
            throw std::runtime_error(
                "Missing KALSHI_API_KEY_ID or KALSHI_PRIVATE_KEY in .env file");
        }

        // Trading configuration
        TradingConfig config;
        config.verbose = (get_env_value(env, "VERBOSE") == "true" ||
                          get_env_value(env, "VERBOSE") == "1");

        // Initialize strategies
        // Chicago strategies (station: KMDW)
        config.strategies.push_back(TradingStrategy(
            "Chicago High Temp >= 3°C",
            "KXHIGHCHI-26JAN16-B35.5",
            "KMDW",  // Chicago Midway
            ComparisonOp::GREATER_EQUAL,
            3,
            5,
            Side::NO,
            99
        ));
        // config.strategies.push_back(TradingStrategy(
        //     "Chicago High Temp >= -3°C",
        //     "KXHIGHCHI-26JAN15-T25",
        //     "KMDW",  // Chicago Midway
        //     ComparisonOp::GREATER_EQUAL,
        //     -3,
        //     5,
        //     Side::NO,
        //     99
        // ));

        // config.strategies.push_back(TradingStrategy(
        //     "Miami High Temp >= 23°C",
        //     "KXHIGHMIA-26JAN15-T73",
        //     "KMIA",  // Miami International
        //     ComparisonOp::GREATER_EQUAL,
        //     23,
        //     5,
        //     Side::NO,
        //     99
        // ));
        // config.strategies.push_back(TradingStrategy(
        //     "Miami Lowest Temp <= 8°C",
        //     "KXLOWTMIA-26JAN15-B48.5",
        //     "KMIA",  // Miami International
        //     ComparisonOp::LESS_EQUAL,
        //     8,
        //     5,
        //     Side::NO,
        //     99
        // ));
        // config.strategies.push_back(TradingStrategy(
        //     "Miami Lowest Temp <= 9°C",
        //     "KXLOWTMIA-26JAN15-B50.5",
        //     "KMIA",  // Miami International
        //     ComparisonOp::LESS_EQUAL,
        //     9,
        //     5,
        //     Side::NO,
        //     99
        // ));
        // config.strategies.push_back(TradingStrategy(
        //     "Miami Lowest Temp <= 10°C",
        //     "KXLOWTMIA-26JAN15-B52.5",
        //     "KMIA",  // Miami International
        //     ComparisonOp::LESS_EQUAL,
        //     10,
        //     5,
        //     Side::NO,
        //     99
        // ));



        // Miami strategies (station: KMIA) - example for multi-location
        // config.strategies.push_back(TradingStrategy(
        //     "Miami Low Temp <= 10°C",
        //     "KXLOWTMIA-26JAN15-B50",
        //     "KMIA",  // Miami International
        //     ComparisonOp::LESS_EQUAL,
        //     10,
        //     5,
        //     Side::NO,
        //     99
        // ));
        // config.strategies.push_back(TradingStrategy(
        //     "Miami High Temp >= 20°C",
        //     "KXHIGHTMIA-26JAN15-T68",
        //     "KMIA",  // Miami International
        //     ComparisonOp::GREATER_EQUAL,
        //     20,
        //     5,
        //     Side::YES,  // Example of buying YES
        //     50
        // ));

        // Collect unique stations from all strategies
        std::set<std::string> unique_stations;
        for (const auto& s : config.strategies) {
            unique_stations.insert(s.station);
        }

        std::cout << "=================================================\n";
        std::cout << "  WEATHER TRADER - Multi-Location\n";
        std::cout << "=================================================\n\n";
        std::cout << "Stations:       ";
        bool first = true;
        for (const auto& stn : unique_stations) {
            if (!first) std::cout << ", ";
            std::cout << stn;
            first = false;
        }
        std::cout << "\n";
        std::cout << "Environment:    " << (is_prod ? "PRODUCTION" : "DEMO") << "\n";
        std::cout << "Poll interval:  " << config.poll_interval_sec << " seconds\n";
        std::cout << "Verbose:        " << (config.verbose ? "ON" : "OFF") << "\n";
        std::cout << "\n";
        std::cout << "Active Strategies:\n";
        for (size_t i = 0; i < config.strategies.size(); ++i) {
            const auto& s = config.strategies[i];
            std::cout << "  " << (i + 1) << ". " << s.name << "\n";
            std::cout << "     Station: " << s.station << "\n";
            std::cout << "     Market: " << s.market_ticker << "\n";
            std::cout << "     Condition: temp "
                      << (s.op == ComparisonOp::LESS_EQUAL ? "<=" : ">=")
                      << " " << s.temp_threshold_c << "°C\n";
            std::cout << "     Order: BUY " << s.order_count
                      << (s.side == Side::YES ? " YES" : " NO") << " @ "
                      << s.price_cents << "c\n";
        }
        std::cout << "\n";

        if (is_prod) {
            std::cout << "*** WARNING: PRODUCTION MODE - REAL MONEY AT RISK ***\n\n";
        }

        // Initialize clients
        madis::MadisClient madis_client;
        std::unordered_map<std::string, madis::FetchState> madis_states;  // State per station
        kalshi::KalshiClient kalshi_client(api_key_id, private_key_path, is_prod);

        std::cout << "Starting polling loop...\n\n";

        while (true) {
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
            localtime_r(&time_t_now, &tm);

            char time_buf[32];
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm);

            // Fetch latest observations for all unique stations
            std::unordered_map<std::string, madis::MadisClient::PollResult> poll_results;
            for (const auto& station : unique_stations) {
                poll_results[station] = madis_client.fetch_latest(station, madis_states[station]);
            }

            // Verbose logging: show every poll
            if (config.verbose) {
                for (const auto& station : unique_stations) {
                    const auto& result = poll_results[station];
                    if (result.not_modified) {
                        std::cout << "[" << time_buf << "] " << station
                                  << " poll: 304 not modified (" << result.filename << ")\n";
                    } else if (result.is_new_data && result.obs) {
                        std::cout << "[" << time_buf << "] " << station
                                  << " poll: 200 NEW DATA (" << result.filename << ")\n";
                    } else if (result.http_status > 0) {
                        std::cout << "[" << time_buf << "] " << station
                                  << " poll: " << result.http_status << " (" << result.filename << ")\n";
                    }
                }
            }

            // Print temperature updates for each station (only when new file downloaded)
            for (const auto& station : unique_stations) {
                const auto& result = poll_results[station];
                if (result.is_new_data && result.obs && result.obs->valid) {
                    // Format observation timestamp
                    std::tm obs_tm{};
                    gmtime_r(&result.obs->obs_time, &obs_tm);
                    char obs_time_buf[32];
                    strftime(obs_time_buf, sizeof(obs_time_buf), "%Y-%m-%d %H:%M:%S UTC", &obs_tm);

                    std::cout << "[" << time_buf << "] "
                              << station << " temp: " << result.obs->temp_c << "°C"
                              << " @ " << obs_time_buf
                              << " (file: " << result.obs->filename << ")\n";
                }
            }

            // Check each strategy independently (use cached obs from state)
            for (auto& strategy : config.strategies) {
                if (strategy.order_placed) continue;  // Already placed order for this strategy

                // Get cached observation for this strategy's station
                const auto& cached = madis_states[strategy.station].cached_obs;
                if (!cached || !cached->valid) {
                    continue;  // No valid data for this station
                }
                const auto& obs = cached.value();

                bool should_trigger = false;
                if (strategy.op == ComparisonOp::LESS_EQUAL) {
                    should_trigger = (obs.temp_c <= strategy.temp_threshold_c);
                } else {
                    should_trigger = (obs.temp_c >= strategy.temp_threshold_c);
                }

                if (should_trigger) {
                    std::string side_str = (strategy.side == Side::YES) ? "YES" : "NO";
                    std::cout << "\n*** PLACING ORDER - " << strategy.name << " ***\n";
                    std::cout << "Station: " << strategy.station << "\n";
                    std::cout << "Buying " << strategy.order_count << " " << side_str << " contracts on "
                              << strategy.market_ticker << " @ " << strategy.price_cents << "c\n";
                    std::cout << "Condition: temp "
                              << (strategy.op == ComparisonOp::LESS_EQUAL ? "<=" : ">=")
                              << " " << strategy.temp_threshold_c << "°C (current: "
                              << obs.temp_c << "°C)\n";
                    std::cout << "Data source: " << obs.filename << "\n";

                    auto result = kalshi_client.buy(
                        strategy.market_ticker,
                        strategy.order_count,
                        strategy.side,
                        strategy.price_cents);

                    if (result.success) {
                        std::cout << "ORDER PLACED! Order ID: " << result.order_id << "\n";
                        std::cout << "Strategy: " << strategy.name << "\n";
                        std::cout << "Data source: " << obs.filename << "\n";
                        strategy.order_placed = true;
                    } else {
                        std::cout << "ORDER FAILED! Status: " << result.status_code << "\n";
                        std::cout << "Response: " << result.raw_response << "\n";
                        std::cout << "Strategy: " << strategy.name << "\n";
                        std::cout << "Data source: " << obs.filename << "\n";
                    }
                    std::cout << "\n";
                }
            }

            // Periodic status update if no valid data for any station
            // Check cached observations, not just current poll results
            bool any_cached = false;
            for (const auto& station : unique_stations) {
                if (madis_states[station].cached_obs) {
                    any_cached = true;
                    break;
                }
            }
            if (!any_cached) {
                static int no_data_count = 0;
                if (++no_data_count >= 15) {  // 15 * 2 seconds = 30 seconds
                    std::cout << "[" << time_buf << "] Waiting for data...\n";
                    no_data_count = 0;
                }
            }

            // Sleep before next poll
            std::this_thread::sleep_for(std::chrono::seconds(config.poll_interval_sec));
        }

        curl_global_cleanup();
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "FATAL ERROR: " << e.what() << "\n";
        return 1;
    }
}
