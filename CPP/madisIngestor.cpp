// madisData.cpp
//
// Low-latency MADIS hourly hfmetar fetcher + netCDF parse to extract latest KMDW rawMessage.
//
// Features:
// - HEAD probes (current UTC hour, then previous hours)
// - ETag + Last-Modified caching
// - Conditional GET (If-None-Match / If-Modified-Since)
// - Download .gz to memory, gunzip in memory
// - Parse netCDF and extract the latest KMDW rawMessage
//
// WSL/Ubuntu setup:
//   sudo apt update
//   sudo apt install -y build-essential libcurl4-openssl-dev zlib1g-dev libnetcdf-dev
//
// Build:
//   g++ -O3 -std=c++20 madisData.cpp -lcurl -lz -lnetcdf -o madisData
//
// Run:
//   ./madisData
//
// Notes on portability:
// - Many Ubuntu/WSL netCDF builds do NOT include nc_open_mem or nc_open_fd.
// - This file uses a portable temp-file-by-path fallback for opening netCDF bytes.

#include <curl/curl.h>
#include <zlib.h>
#include <netcdf.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// temp-file helpers (portable)
#include <unistd.h>   // mkstemp, unlink, close, write
#include <fcntl.h>    // open flags (mkstemp)
#include <sys/stat.h> // not strictly required

namespace madis {

static constexpr std::string_view kBaseUrl =
    "https://madis-data.ncep.noaa.gov/madisPublic1/data/LDAD/hfmetar/netCDF";

// ============================== HTTP helpers ==============================

struct HttpMeta {
  long status = 0;
  std::string etag;
  std::string last_modified;
  std::uint64_t content_length = 0;
};

static inline std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
    s.pop_back();
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  if (i) s.erase(0, i);
  return s;
}

static inline bool istarts_with(std::string_view s, std::string_view p) {
  if (s.size() < p.size()) return false;
  for (std::size_t i = 0; i < p.size(); i++) {
    char a = s[i], b = p[i];
    if ('A' <= a && a <= 'Z') a = char(a - 'A' + 'a');
    if ('A' <= b && b <= 'Z') b = char(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

static size_t write_to_vec(void* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::vector<std::uint8_t>*>(userdata);
  const size_t bytes = size * nmemb;
  const auto* p = static_cast<std::uint8_t*>(ptr);
  out->insert(out->end(), p, p + bytes);
  return bytes;
}

static size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
  auto* meta = static_cast<HttpMeta*>(userdata);
  const size_t bytes = size * nitems;
  std::string line(buffer, buffer + bytes);
  std::string_view sv(line);

  if (istarts_with(sv, "etag:")) {
    meta->etag = trim(line.substr(5));
  } else if (istarts_with(sv, "last-modified:")) {
    meta->last_modified = trim(line.substr(14));
  } else if (istarts_with(sv, "content-length:")) {
    const std::string v = trim(line.substr(15));
    try { meta->content_length = std::stoull(v); } catch (...) {}
  }
  return bytes;
}

// ============================== gunzip ==============================

static std::vector<std::uint8_t> gunzip_or_throw(const std::vector<std::uint8_t>& gz) {
  if (gz.size() < 2) throw std::runtime_error("gunzip: input too small");

  z_stream zs{};
  if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {
    throw std::runtime_error("gunzip: inflateInit2 failed");
  }

  zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(gz.data()));
  zs.avail_in = static_cast<uInt>(gz.size());

  std::vector<std::uint8_t> out;
  out.reserve(gz.size() * 3);

  std::uint8_t buf[1 << 15];
  int ret = Z_OK;

  while (ret != Z_STREAM_END) {
    zs.next_out = buf;
    zs.avail_out = sizeof(buf);
    ret = inflate(&zs, Z_NO_FLUSH);

    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&zs);
      throw std::runtime_error("gunzip: inflate failed with code " + std::to_string(ret));
    }

    const std::size_t produced = sizeof(buf) - zs.avail_out;
    out.insert(out.end(), buf, buf + produced);
  }

  inflateEnd(&zs);
  return out;
}

// ============================== libcurl RAII ==============================

class CurlGlobal {
 public:
  CurlGlobal() {
    if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
      throw std::runtime_error("curl_global_init failed");
    }
  }
  ~CurlGlobal() { curl_global_cleanup(); }
};

class HttpClient {
 public:
  HttpClient() {
    h_ = curl_easy_init();
    if (!h_) throw std::runtime_error("curl_easy_init failed");

    curl_easy_setopt(h_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h_, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(h_, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(h_, CURLOPT_TCP_KEEPINTVL, 15L);

    // If host+lib support HTTP/2 over TLS, may reduce handshake overhead.
    curl_easy_setopt(h_, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(h_, CURLOPT_FOLLOWLOCATION, 1L);

    curl_easy_setopt(h_, CURLOPT_CONNECTTIMEOUT_MS, 1200L);
    curl_easy_setopt(h_, CURLOPT_TIMEOUT_MS, 4000L);
  }

  ~HttpClient() {
    if (h_) curl_easy_cleanup(h_);
  }

  HttpMeta head(const std::string& url) {
    HttpMeta meta{};
    curl_easy_reset(h_);

    curl_easy_setopt(h_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h_, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(h_, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h_, CURLOPT_HEADERDATA, &meta);
    curl_easy_setopt(h_, CURLOPT_FAILONERROR, 0L);

    CURLcode rc = curl_easy_perform(h_);
    if (rc != CURLE_OK) return meta;

    curl_easy_getinfo(h_, CURLINFO_RESPONSE_CODE, &meta.status);
    return meta;
  }

  struct GetResult {
    HttpMeta meta;
    std::vector<std::uint8_t> body; // gz bytes
    bool not_modified = false;
  };

  GetResult get_conditional(const std::string& url,
                            const std::string& etag,
                            const std::string& last_modified) {
    GetResult gr{};
    curl_easy_reset(h_);

    curl_easy_setopt(h_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h_, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(h_, CURLOPT_WRITEFUNCTION, write_to_vec);
    curl_easy_setopt(h_, CURLOPT_WRITEDATA, &gr.body);
    curl_easy_setopt(h_, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h_, CURLOPT_HEADERDATA, &gr.meta);
    curl_easy_setopt(h_, CURLOPT_FAILONERROR, 0L);

    struct curl_slist* headers = nullptr;
    if (!etag.empty()) {
      std::string h = "If-None-Match: " + etag;
      headers = curl_slist_append(headers, h.c_str());
    }
    if (!last_modified.empty()) {
      std::string h = "If-Modified-Since: " + last_modified;
      headers = curl_slist_append(headers, h.c_str());
    }
    if (headers) curl_easy_setopt(h_, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(h_);
    if (headers) curl_slist_free_all(headers);

    if (rc != CURLE_OK) return gr;

    curl_easy_getinfo(h_, CURLINFO_RESPONSE_CODE, &gr.meta.status);
    if (gr.meta.status == 304) {
      gr.not_modified = true;
      gr.body.clear();
    }
    return gr;
  }

 private:
  CURL* h_ = nullptr;
};

// ============================== hourly filenames (UTC) ==============================

static std::string make_hourly_name_utc(std::time_t utc_tt) {
  std::tm t{};
#if defined(_WIN32)
  gmtime_s(&t, &utc_tt);
#else
  gmtime_r(&utc_tt, &t);
#endif

  char buf[64];
  const int n = std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d00.gz",
                              t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);
  if (n < 0 || static_cast<std::size_t>(n) >= sizeof(buf)) {
    throw std::runtime_error("make_hourly_name_utc: snprintf failed/truncated");
  }
  return std::string(buf);
}

static std::vector<std::string> hourly_candidate_urls(const std::string& base_url,
                                                      int lookback_hours = 3) {
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(lookback_hours));

  const std::time_t now = std::time(nullptr);

  std::tm now_tm{};
#if defined(_WIN32)
  gmtime_s(&now_tm, &now);
#else
  gmtime_r(&now, &now_tm);
#endif

  std::time_t floored = now - (now_tm.tm_min * 60) - now_tm.tm_sec;

  for (int i = 0; i < lookback_hours; ++i) {
    const std::time_t tt = floored - (i * 3600);
    out.push_back(base_url + "/" + make_hourly_name_utc(tt));
  }
  return out;
}

// ============================== fetcher ==============================

struct FetchState {
  std::string last_url;
  std::string last_etag;
  std::string last_modified;
};

struct FetchedNetcdf {
  std::string url;
  HttpMeta meta;
  std::vector<std::uint8_t> netcdf; // decompressed bytes
};

class MadisFetcher {
 public:
  std::optional<std::pair<std::string, HttpMeta>> find_latest_candidate(int lookback_hours = 3) {
    auto urls = hourly_candidate_urls(std::string(kBaseUrl), lookback_hours);
    for (const auto& url : urls) {
      HttpMeta m = http_.head(url);
      if (m.status == 200) return std::make_pair(url, std::move(m));
    }
    return std::nullopt;
  }

  std::optional<FetchedNetcdf> fetch_if_new(const std::string& url, FetchState& st) {
    auto gr = http_.get_conditional(url, st.last_etag, st.last_modified);
    if (gr.meta.status == 304 || gr.not_modified) return std::nullopt;
    if (gr.meta.status != 200 || gr.body.empty()) return std::nullopt;

    auto netcdf = gunzip_or_throw(gr.body);

    st.last_url = url;
    if (!gr.meta.etag.empty()) st.last_etag = gr.meta.etag;
    if (!gr.meta.last_modified.empty()) st.last_modified = gr.meta.last_modified;

    return FetchedNetcdf{.url = url, .meta = std::move(gr.meta), .netcdf = std::move(netcdf)};
  }

 private:
  HttpClient http_;
};

} // namespace madis

// ============================== netCDF utils ==============================

static void nc_check(int rc, const char* what) {
  if (rc != NC_NOERR) throw std::runtime_error(std::string(what) + ": " + nc_strerror(rc));
}

static std::string trim_right(std::string s) {
  while (!s.empty() && (s.back() == '\0' || s.back() == ' ' || s.back() == '\n' ||
                        s.back() == '\r' || s.back() == '\t')) {
    s.pop_back();
  }
  return s;
}

struct Char2D {
  size_t n = 0;
  size_t s = 0;
  std::vector<char> buf; // n*s
};

static std::optional<int> try_varid(int ncid, const std::initializer_list<const char*>& names) {
  for (auto* name : names) {
    int varid = -1;
    if (nc_inq_varid(ncid, name, &varid) == NC_NOERR) return varid;
  }
  return std::nullopt;
}

static Char2D read_char2d(int ncid, int varid) {
  int ndims = 0;
  int dimids[NC_MAX_VAR_DIMS]{};
  nc_type vartype{};
  nc_check(nc_inq_var(ncid, varid, nullptr, &vartype, &ndims, dimids, nullptr), "nc_inq_var");
  if (vartype != NC_CHAR || ndims != 2) throw std::runtime_error("Expected 2D NC_CHAR variable");

  size_t n0 = 0, n1 = 0;
  nc_check(nc_inq_dimlen(ncid, dimids[0], &n0), "nc_inq_dimlen dim0");
  nc_check(nc_inq_dimlen(ncid, dimids[1], &n1), "nc_inq_dimlen dim1");

  Char2D out;
  out.n = n0;
  out.s = n1;
  out.buf.resize(out.n * out.s);

  nc_check(nc_get_var_text(ncid, varid, out.buf.data()), "nc_get_var_text");
  return out;
}

static std::optional<std::vector<double>> read_time_vector(int ncid, size_t expected_n) {
  auto vid_opt = try_varid(ncid, {"timeObs", "obsTime", "time_observation", "observationTime"});
  if (!vid_opt) return std::nullopt;
  int varid = *vid_opt;

  nc_type t{};
  int ndims = 0;
  int dimids[NC_MAX_VAR_DIMS]{};
  nc_check(nc_inq_var(ncid, varid, nullptr, &t, &ndims, dimids, nullptr), "nc_inq_var time");
  if (ndims != 1) return std::nullopt;

  size_t n = 0;
  nc_check(nc_inq_dimlen(ncid, dimids[0], &n), "nc_inq_dimlen time");
  if (expected_n != 0 && n != expected_n) return std::nullopt;

  std::vector<double> out(n);
  int rc = nc_get_var_double(ncid, varid, out.data());
  if (rc != NC_NOERR) return std::nullopt;
  return out;
}

static std::string get_row_string(const Char2D& a, size_t i) {
  const char* p = a.buf.data() + i * a.s;
  size_t len = 0;
  while (len < a.s && p[len] != '\0') len++;
  std::string s(p, p + len);
  return trim_right(std::move(s));
}

static bool rawmessage_mentions_kmdw(const std::string& msg) {
  if (msg.find(" KMDW ") != std::string::npos) return true;
  if (msg.rfind("METAR KMDW", 0) == 0) return true;
  if (msg.rfind("SPECI KMDW", 0) == 0) return true;
  if (msg.find("METAR KMDW") != std::string::npos) return true;
  if (msg.find("SPECI KMDW") != std::string::npos) return true;
  return false;
}

// ============================== temp-file open for netCDF bytes ==============================

struct TempPathFile {
  std::string path;
  ~TempPathFile() {
    if (!path.empty()) ::unlink(path.c_str());
  }
};

static TempPathFile write_bytes_to_temp_path(const std::vector<std::uint8_t>& bytes) {
  char tmpl[] = "/tmp/madisXXXXXX";
  int fd = ::mkstemp(tmpl);
  if (fd < 0) throw std::runtime_error("mkstemp failed");

  size_t written = 0;
  while (written < bytes.size()) {
    ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (n <= 0) {
      ::close(fd);
      ::unlink(tmpl);
      throw std::runtime_error("write(tempfile) failed");
    }
    written += static_cast<size_t>(n);
  }

  ::close(fd);

  TempPathFile tpf;
  tpf.path = tmpl;
  return tpf;
}

// ============================== extract latest KMDW rawMessage ==============================

static std::optional<std::string> extract_latest_kmdw_rawMessage_from_netcdf_bytes(
    const std::vector<std::uint8_t>& netcdf_bytes) {

  // Write netCDF bytes to temp path (portable on WSL), open with nc_open().
  TempPathFile tmp = write_bytes_to_temp_path(netcdf_bytes);

  int ncid = -1;
  int rc = nc_open(tmp.path.c_str(), NC_NOWRITE, &ncid);
  if (rc != NC_NOERR) {
    throw std::runtime_error(std::string("nc_open failed: ") + nc_strerror(rc));
  }

  auto close_guard = [&]() {
    if (ncid >= 0) nc_close(ncid);
  };

  try {
    // rawMessage
    int raw_varid = -1;
    nc_check(nc_inq_varid(ncid, "rawMessage", &raw_varid), "nc_inq_varid rawMessage");
    Char2D raw = read_char2d(ncid, raw_varid);

    // stationId (preferred for matching)
    std::optional<Char2D> station;
    if (auto sid = try_varid(ncid, {"stationId", "stationID", "station_id"})) {
      try {
        station = read_char2d(ncid, *sid);
        if (station->n != raw.n) station.reset();
      } catch (...) {
        station.reset();
      }
    }

    // timeObs (preferred for "latest")
    std::optional<std::vector<double>> timev = read_time_vector(ncid, raw.n);

    std::optional<size_t> best_idx;
    double best_time = -1e300;

    for (size_t i = 0; i < raw.n; i++) {
      bool is_kmdw = false;

      if (station) {
        const std::string st = get_row_string(*station, i);
        is_kmdw = (st == "KMDW");
      } else {
        const std::string msg = get_row_string(raw, i);
        is_kmdw = rawmessage_mentions_kmdw(msg);
      }

      if (!is_kmdw) continue;

      if (timev) {
        const double t = (*timev)[i];
        if (!best_idx || t > best_time) {
          best_time = t;
          best_idx = i;
        }
      } else {
        best_idx = i; // fallback: last match
      }
    }

    if (!best_idx) {
      close_guard();
      return std::nullopt;
    }

    const std::string best_msg = get_row_string(raw, *best_idx);
    close_guard();
    return best_msg;
  } catch (...) {
    close_guard();
    throw;
  }
}

// ===================================== main =====================================

int main() {
  try {
    madis::CurlGlobal cg;

    madis::MadisFetcher f;
    madis::FetchState st;

    // Look back a few hours in case the current hour isn't published yet.
    auto latest = f.find_latest_candidate(/*lookback_hours=*/3);
    if (!latest) {
      std::cerr << "No candidate found (yet)\n";
      return 0;
    }

    const auto& [url, meta] = *latest;
    std::cerr << "Candidate: " << url << "\n";
    std::cerr << "HEAD status=" << meta.status
              << " etag=" << meta.etag
              << " lastmod=" << meta.last_modified
              << " len=" << meta.content_length << "\n";

    auto fetched = f.fetch_if_new(url, st);
    if (!fetched) {
      std::cerr << "Not modified / failed to fetch\n";
      return 0;
    }

    std::cerr << "Fetched + decompressed netCDF bytes: " << fetched->netcdf.size() << "\n";

    auto kmdw = extract_latest_kmdw_rawMessage_from_netcdf_bytes(fetched->netcdf);
    if (!kmdw) {
      std::cerr << "No KMDW rawMessage found in this file\n";
      return 0;
    }

    std::cout << "LATEST KMDW METAR:\n" << *kmdw << "\n";
    return 0;

  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
