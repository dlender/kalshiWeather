// madis_client.hpp
// MADIS METAR data fetching and parsing

#pragma once

#include "common.hpp"

#include <curl/curl.h>
#include <zlib.h>
#include <netcdf.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <vector>

// temp-file helpers (POSIX)
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

namespace madis {

static constexpr std::string_view kBaseUrl =
    "https://madis-data.ncep.noaa.gov/madisPublic1/data/LDAD/hfmetar/netCDF";

// ============================================================================
// HTTP HELPERS
// ============================================================================

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

// ============================================================================
// GZIP DECOMPRESSION
// ============================================================================

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

// ============================================================================
// MADIS OBSERVATION
// ============================================================================

struct Observation {
    std::string station;
    std::time_t obs_time;
    int temp_c;           // Temperature in Celsius
    int dewpoint_c;       // Dewpoint in Celsius
    std::string raw_metar;
    std::string filename; // Name of the MADIS file used
    bool valid = false;
};

// ============================================================================
// FETCH STATE (for conditional GET)
// ============================================================================

struct FetchState {
    std::string last_url;
    std::string last_etag;
    std::string last_modified;
    int last_hour = -1;
    std::optional<Observation> cached_obs;
};

// ============================================================================
// MADIS CLIENT
// ============================================================================

class MadisClient {
public:
    MadisClient() {
        curl_ = curl_easy_init();
        if (!curl_) throw std::runtime_error("curl_easy_init failed");
    }

    ~MadisClient() {
        if (curl_) curl_easy_cleanup(curl_);
    }

    // Result of a fetch operation
    struct PollResult {
        std::optional<Observation> obs;
        bool is_new_data = false;
        bool not_modified = false;
        int http_status = 0;
        std::string filename;
    };

    // Fetch latest observation for a station
    PollResult fetch_latest(const std::string& station, FetchState& state) {
        auto now = std::time(nullptr);
        std::tm tm{};
        gmtime_r(&now, &tm);
        std::time_t floored = now - (tm.tm_min * 60) - tm.tm_sec;
        int current_hour = tm.tm_hour;

        // If hour changed, clear cached state
        if (state.last_hour != -1 && state.last_hour != current_hour) {
            state.last_url.clear();
            state.last_etag.clear();
            state.last_modified.clear();
            state.cached_obs = std::nullopt;
        }

        // Try current hour and previous hours
        for (int i = 0; i < 3; ++i) {
            std::time_t tt = floored - (i * 3600);
            std::tm t{};
            gmtime_r(&tt, &t);

            char filename[64];
            snprintf(filename, sizeof(filename), "%04d%02d%02d_%02d00.gz",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);

            std::string url = std::string(kBaseUrl) + "/" + filename;

            auto result = try_fetch_url(url, station, state, filename);

            if (result.new_obs && result.new_obs->valid) {
                state.cached_obs = result.new_obs;
                state.last_hour = t.tm_hour;
                return {result.new_obs, true, false, result.http_status, result.filename};
            }

            if (result.not_modified && state.cached_obs) {
                return {std::nullopt, false, true, 304, result.filename};
            }

            // Don't fall back to older files if we have cached data
            if (state.cached_obs) {
                return {std::nullopt, false, false, result.http_status, result.filename};
            }
        }
        return {std::nullopt, false, false, 0, ""};
    }

    // Build ObservationEvent from Observation (for NATS publishing)
    ObservationEvent to_event(const Observation& obs) {
        ObservationEvent evt;
        evt.event_type = "metar_observation";
        evt.source = "madis_hfmetar";
        evt.station = obs.station;
        evt.obs_time_utc = obs.obs_time;
        evt.recv_time_utc = std::time(nullptr);
        evt.temp_c = obs.temp_c;
        evt.dewpoint_c = obs.dewpoint_c;
        evt.raw_metar = obs.raw_metar;

        // Generate dedupe key
        std::ostringstream key;
        key << evt.source << "_" << evt.station << "_" << evt.obs_time_utc;
        evt.dedupe_key = key.str();

        return evt;
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
            return {std::nullopt, true, 304, filename};
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

    // Parse field from rawMessage CSV string
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

    static int parse_temp_from_raw(const std::string& raw) {
        std::string val = parse_field_from_raw(raw, 15);
        try {
            return val.empty() ? -999 : std::stoi(val);
        } catch (...) {
            return -999;
        }
    }

    // Parse time from rawMessage (field 2): "YY/MM/DD HH:MM:SS"
    static std::time_t parse_time_from_raw(const std::string& raw) {
        std::string val = parse_field_from_raw(raw, 2);
        if (val.size() < 17) return 0;

        try {
            std::tm tm{};
            tm.tm_year = std::stoi(val.substr(0, 2)) + 100;  // YY -> years since 1900
            tm.tm_mon = std::stoi(val.substr(3, 2)) - 1;
            tm.tm_mday = std::stoi(val.substr(6, 2));
            tm.tm_hour = std::stoi(val.substr(9, 2));
            tm.tm_min = std::stoi(val.substr(12, 2));
            tm.tm_sec = std::stoi(val.substr(15, 2));
            return timegm(&tm);
        } catch (...) {
            return 0;
        }
    }

    std::optional<Observation> parse_netcdf(const std::vector<std::uint8_t>& data,
                                             const std::string& target_station) {
        // Write to temp file
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

            // Get dimensions
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
                while (!station.empty() && (station.back() == ' ' || station.back() == '\0'))
                    station.pop_back();

                if (station != target_station) continue;

                double obs_time = times[i];
                std::string raw_metar;
                if (!raw_messages.empty()) {
                    raw_metar = std::string(&raw_messages[i * raw_len], raw_len);
                    while (!raw_metar.empty() &&
                           (raw_metar.back() == ' ' || raw_metar.back() == '\0'))
                        raw_metar.pop_back();

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
