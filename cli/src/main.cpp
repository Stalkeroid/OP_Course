// Command line client for the search server (REST only, see docs/API.md).
#include <httplib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using nlohmann::json;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitError = 2;

struct Options {
  std::string host = "localhost";
  int port = 8080;
  std::optional<int> limit;
  bool noExpand = false;
  bool rawJson = false;
  bool doIndex = false;
  std::optional<int> indexThreads;
  bool doStats = false;
  bool doHealth = false;
  int timeoutSec = 120;
  bool help = false;
  std::string query;
};

void printHelp(std::ostream& out) {
  out << "Usage: search_cli [options] \"query words\"\n"
         "\n"
         "Search the indexed documents through the REST API of the server.\n"
         "\n"
         "Options:\n"
         "  --host H       server host (default: localhost)\n"
         "  --port P       server port (default: $PORT, else 8080)\n"
         "  --limit N      number of results, 1..100 (server default: 10)\n"
         "  --no-expand    skip LLM query expansion (sends \"expand\": false)\n"
         "  --json         print the raw response body and nothing else\n"
         "  --index [N]    build the index (POST /api/index), optionally with "
         "N "
         "threads\n"
         "  --stats        show index statistics (GET /api/stats)\n"
         "  --health       show server health (GET /api/health)\n"
         "  --timeout S    read timeout in seconds (default: 120; "
         "3600 for --index)\n"
         "  --help         show this help\n"
         "\n"
         "Exit codes: 0 ok, 1 usage error, 2 server/API/network error.\n"
         "Examples:\n"
         "  ./search --index 4\n"
         "  ./search \"climate change\" --limit 5\n";
}

bool parseInt(const std::string& s, int& out) {
  if (s.empty() || s.size() > 9) return false;
  size_t i = (s[0] == '-') ? 1 : 0;
  if (i == s.size()) return false;
  for (size_t k = i; k < s.size(); ++k) {
    if (s[k] < '0' || s[k] > '9') return false;
  }
  out = std::stoi(s);
  return true;
}

[[noreturn]] void usageError(const std::string& msg) {
  std::cerr << "error: usage: " << msg << "\nTry 'search_cli --help'.\n";
  std::exit(kExitUsage);
}

bool looksLikeInt(const char* s) {
  int v;
  return parseInt(s, v);
}

Options parseArgs(int argc, char** argv) {
  Options o;
  if (const char* env = std::getenv("PORT")) {
    int p;
    if (parseInt(env, p) && p >= 1 && p <= 65535) o.port = p;
  }
  std::vector<std::string> words;
  bool timeoutGiven = false;
  auto needValue = [&](int& i, const std::string& name) -> std::string {
    if (i + 1 >= argc) usageError(name + " requires a value");
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      o.help = true;
    } else if (a == "--host") {
      o.host = needValue(i, a);
    } else if (a == "--port") {
      int p;
      if (!parseInt(needValue(i, a), p) || p < 1 || p > 65535)
        usageError("--port must be an integer in [1, 65535]");
      o.port = p;
    } else if (a == "--limit") {
      int n;
      if (!parseInt(needValue(i, a), n) || n < 1 || n > 100)
        usageError("--limit must be an integer in [1, 100]");
      o.limit = n;
    } else if (a == "--no-expand") {
      o.noExpand = true;
    } else if (a == "--json") {
      o.rawJson = true;
    } else if (a == "--index") {
      o.doIndex = true;
      if (i + 1 < argc && looksLikeInt(argv[i + 1])) {
        int n;
        parseInt(argv[++i], n);
        if (n < 1 || n > 64) usageError("--index N must be in [1, 64]");
        o.indexThreads = n;
      }
    } else if (a == "--stats") {
      o.doStats = true;
    } else if (a == "--health") {
      o.doHealth = true;
    } else if (a == "--timeout") {
      int s;
      if (!parseInt(needValue(i, a), s) || s < 1)
        usageError("--timeout must be a positive integer");
      o.timeoutSec = s;
      timeoutGiven = true;
    } else if (a == "--") {
      for (++i; i < argc; ++i) words.push_back(argv[i]);
    } else if (a.size() > 1 && a[0] == '-' && a[1] == '-') {
      usageError("unknown option " + a);
    } else {
      words.push_back(a);
    }
  }
  if (o.doIndex && !timeoutGiven) o.timeoutSec = 3600;
  for (size_t i = 0; i < words.size(); ++i) {
    if (i) o.query += ' ';
    o.query += words[i];
  }
  if (o.help) return o;
  int actions =
      int(o.doIndex) + int(o.doStats) + int(o.doHealth) + int(!o.query.empty());
  if (actions == 0)
    usageError("a query or one of --index/--stats/--health is required");
  if (actions > 1)
    usageError(
        "give either a query or exactly one of --index/--stats/--health");
  return o;
}

int terminalWidth() {
  struct winsize ws {};
  if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
      ws.ws_col >= 40)
    return std::min<int>(ws.ws_col, 200);
  if (const char* c = std::getenv("COLUMNS")) {
    int v;
    if (parseInt(c, v) && v >= 40) return std::min(v, 200);
  }
  return 100;
}

// Wraps text to the given width; stops after maxLines and appends "...".
std::vector<std::string> wrap(const std::string& text, size_t width,
                              size_t maxLines) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string word, cur;
  bool truncated = false;
  while (in >> word) {
    if (!cur.empty() && cur.size() + 1 + word.size() > width) {
      lines.push_back(cur);
      cur.clear();
      if (lines.size() >= maxLines) {
        truncated = true;
        break;
      }
    }
    if (!cur.empty()) cur += ' ';
    cur += word;
  }
  if (!truncated && !cur.empty()) lines.push_back(cur);
  if (truncated && !lines.empty()) lines.back() += " ...";
  return lines;
}

std::string fmtScore(double s) {
  std::ostringstream o;
  o << std::fixed << std::setprecision(2) << s;
  return o.str();
}

std::string valueOr(const json& j, const char* key, const std::string& d) {
  if (j.is_object() && j.contains(key) && j[key].is_string())
    return j[key].get<std::string>();
  return d;
}

// Prints "error: <code>: <message>" and returns the exit code.
int reportApiError(int status, const std::string& body) {
  json j = json::parse(body, nullptr, false);
  if (j.is_object() && j.contains("code")) {
    std::string code = valueOr(j, "code", "unknown");
    std::cerr << "error: " << code << ": "
              << valueOr(j, "error", "(no message)") << "\n";
    if (code == "index_not_built")
      std::cerr << "hint: build the index first, e.g. ./search --index 4\n";
    if (code == "index_in_progress")
      std::cerr << "hint: an index build is already running; retry later\n";
  } else {
    std::cerr << "error: http_" << status
              << ": unexpected response from server\n";
  }
  return kExitError;
}

int reportNetworkError(const Options& o, httplib::Error e) {
  std::cerr << "error: unreachable: cannot get a response from " << o.host
            << ":" << o.port << " (" << httplib::to_string(e) << ")\n";
  if (e == httplib::Error::Connection)
    std::cerr << "hint: is the server running? try 'docker compose up'\n";
  if (e == httplib::Error::Read)
    std::cerr << "hint: the request may have timed out; see --timeout\n";
  return kExitError;
}

// Performs the request; returns exit code on failure, nullopt on HTTP 200.
std::optional<int> doRequest(const Options& o, const std::string& method,
                             const std::string& path, const json& body,
                             std::string& out) {
  httplib::Client cli(o.host, o.port);
  cli.set_connection_timeout(5);
  cli.set_read_timeout(o.timeoutSec);
  cli.set_write_timeout(30);
  httplib::Result res = method == "GET"
                            ? cli.Get(path)
                            : cli.Post(path, body.dump(), "application/json");
  if (!res) return reportNetworkError(o, res.error());
  if (res->status != 200) return reportApiError(res->status, res->body);
  out = res->body;
  return std::nullopt;
}

int64_t intOr(const json& j, const char* key, int64_t d = 0) {
  if (j.is_object() && j.contains(key) && j[key].is_number_integer())
    return j[key].get<int64_t>();
  return d;
}

int showSearch(const Options& o, const json& r) {
  const int width = terminalWidth();
  std::string expansion = valueOr(r, "expansion", "?");
  std::cout << "Query: " << o.query << "\n";
  std::cout << "Expanded terms (" << expansion << "): ";
  if (r.contains("expanded_terms") && r["expanded_terms"].is_array()) {
    bool first = true;
    for (const auto& t : r["expanded_terms"]) {
      if (!t.is_string()) continue;
      std::cout << (first ? "" : ", ") << t.get<std::string>();
      first = false;
    }
  }
  std::cout << "\n\n";
  const json& results = r.contains("results") ? r["results"] : json::array();
  if (!results.is_array() || results.empty()) {
    std::cout << "No results.\n";
  } else {
    int rank = 1;
    for (const auto& item : results) {
      std::cout << std::setw(2) << rank++ << ". ["
                << fmtScore(item.value("score", 0.0)) << "] "
                << valueOr(item, "doc_id", "?") << "\n";
      for (const auto& line : wrap(valueOr(item, "snippet", ""), width - 7, 3))
        std::cout << "      " << line << "\n";
      std::cout << "\n";
    }
  }
  std::cout << "total_matches: " << intOr(r, "total_matches") << " | search "
            << intOr(r, "search_time_ms") << " ms | total "
            << intOr(r, "time_ms") << " ms\n";
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  Options o = parseArgs(argc, argv);
  if (o.help) {
    printHelp(std::cout);
    return kExitOk;
  }

  std::string method = "POST", path, body;
  json req = json::object();
  if (o.doHealth) {
    method = "GET";
    path = "/api/health";
  } else if (o.doStats) {
    method = "GET";
    path = "/api/stats";
  } else if (o.doIndex) {
    path = "/api/index";
    if (o.indexThreads) req["threads"] = *o.indexThreads;
  } else {
    path = "/api/search";
    req["query"] = o.query;
    req["mode"] = "keyword";
    if (o.limit) req["limit"] = *o.limit;
    if (o.noExpand) req["expand"] = false;
  }

  if (auto rc = doRequest(o, method, path, req, body)) return *rc;

  if (o.rawJson) {
    std::cout << body << "\n";
    return kExitOk;
  }
  json r = json::parse(body, nullptr, false);
  if (r.is_discarded()) {
    std::cerr << "error: invalid_response: server returned malformed JSON\n";
    return kExitError;
  }
  auto yn = [&](const char* k) { return r.value(k, false) ? "yes" : "no"; };
  if (o.doHealth) {
    std::cout << "Server:   " << valueOr(r, "status", "?") << "\n"
              << "Ollama:   "
              << (r.value("ollama", false) ? "ready" : "not ready") << "\n"
              << "Index:    "
              << (r.value("index_ready", false) ? "ready" : "not built") << "\n"
              << "Indexing: " << yn("indexing") << "\n";
    return kExitOk;
  }
  if (o.doStats) {
    std::cout << "Documents:        " << intOr(r, "documents") << "\n"
              << "Unique terms:     " << intOr(r, "unique_terms") << "\n"
              << "Last build time:  " << intOr(r, "index_build_time_ms")
              << " ms\n"
              << "Threads used:     " << intOr(r, "threads_used") << "\n"
              << "Index ready:      " << yn("index_ready") << "\n"
              << "Indexing now:     " << yn("indexing") << "\n";
    return kExitOk;
  }
  if (o.doIndex) {
    std::cout << "Index built: " << intOr(r, "documents_indexed")
              << " documents in " << intOr(r, "time_ms") << " ms using "
              << intOr(r, "threads_used") << " threads";
    if (intOr(r, "files_skipped") > 0)
      std::cout << " (" << intOr(r, "files_skipped") << " files skipped)";
    std::cout << "\n";
    return kExitOk;
  }
  return showSearch(o, r);
}
