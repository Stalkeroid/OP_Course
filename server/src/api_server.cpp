#include "search/api_server.h"

#include <crow.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <variant>

namespace search {

using nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr char kJson[] = "application/json";
constexpr std::size_t kMaxWebSocketPayload = 65536;  // §10.6
// Crow drops a connection WITHOUT a close frame when its own limit is hit, so
// its limit is set higher and frames above kMaxWebSocketPayload are rejected
// in onmessage with a proper close frame (code 1009, message too big).
constexpr std::uint64_t kCrowWebSocketLimit = 1 << 20;

std::string dumpJson(const json& value) {
  return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

HttpReply jsonReply(int status, const json& body) {
  return HttpReply{status, kJson, dumpJson(body)};
}

HttpReply errorReply(const ApiError& error) {
  return jsonReply(error.httpStatus, restErrorJson(error));
}

HttpReply internalErrorReply() {
  return errorReply(
      ApiError{500, "internal_error", "unexpected internal error"});
}

bool startsWith(const std::string& text, const char* prefix) {
  return text.rfind(prefix, 0) == 0;
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Percent-decodes a URL path. Returns false on a malformed escape.
bool percentDecode(const std::string& in, std::string& out) {
  out.clear();
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '%') {
      out.push_back(in[i]);
      continue;
    }
    if (i + 2 >= in.size()) return false;
    const int hi = hexValue(in[i + 1]);
    const int lo = hexValue(in[i + 2]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<char>(hi * 16 + lo));
    i += 2;
  }
  return true;
}

const char* contentTypeFor(const fs::path& file) {
  const std::string ext = file.extension().string();
  if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
  if (ext == ".css") return "text/css; charset=utf-8";
  if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
  if (ext == ".json" || ext == ".map") return "application/json";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".png") return "image/png";
  if (ext == ".ico") return "image/x-icon";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".gif") return "image/gif";
  if (ext == ".webp") return "image/webp";
  if (ext == ".woff2") return "font/woff2";
  if (ext == ".txt") return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

HttpReply notFoundPlain() {
  return HttpReply{404, "text/plain; charset=utf-8", "404 Not Found\n"};
}

std::int64_t elapsedMs(Clock::time_point since) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                               since)
      .count();
}

}  // namespace

struct ApiServer::Impl {
  crow::SimpleApp app;
  std::mutex sessionsMutex;
  std::unordered_map<crow::websocket::connection*,
                     std::shared_ptr<WebSocketSession>>
      sessions;
  std::mutex logMutex;

  void logLine(const std::string& method, const std::string& path, int status,
               std::int64_t ms) {
    std::lock_guard<std::mutex> lock(logMutex);
    std::printf("%s %s -> %d (%lld ms)\n", method.c_str(), path.c_str(), status,
                static_cast<long long>(ms));
    std::fflush(stdout);
  }
};

ApiServer::ApiServer(Config config,
                     std::shared_ptr<SearchPipelineService> service)
    : config_(std::move(config)),
      service_(std::move(service)),
      impl_(std::make_unique<Impl>()) {}

ApiServer::~ApiServer() = default;

HttpReply ApiServer::handleHealthCheck() {
  try {
    const bool ollama = service_->ollamaHealthy();
    const ServiceStatus status = service_->status();
    return jsonReply(200, {{"status", "ok"},
                           {"ollama", ollama},
                           {"index_ready", status.indexReady},
                           {"indexing", status.indexing}});
  } catch (...) {
    return internalErrorReply();
  }
}

HttpReply ApiServer::handleIndexPost(const std::string& body) {
  try {
    auto parsed = parseIndexRequest(body, defaultThreadCount());
    if (const auto* error = std::get_if<ApiError>(&parsed)) {
      return errorReply(*error);
    }
    const int threads = std::get<IndexRequest>(parsed).threads;
    const BuildResult result = service_->buildIndex(threads);
    if (result.error) return errorReply(*result.error);
    return jsonReply(200, {{"status", "ok"},
                           {"documents_indexed", result.stats.documents},
                           {"time_ms", result.stats.indexBuildTimeMs},
                           {"threads_used", result.stats.threadsUsed},
                           {"files_skipped", result.stats.filesSkipped}});
  } catch (...) {
    return internalErrorReply();
  }
}

HttpReply ApiServer::handleSearchPost(const std::string& body) {
  try {
    auto parsed = parseSearchRequest(body, MessageSource::kHttpBody);
    if (const auto* error = std::get_if<ApiError>(&parsed)) {
      return errorReply(*error);
    }
    const SearchRequest& request = std::get<SearchRequest>(parsed);
    const auto started = Clock::now();  // validated request -> built response
    auto outcome = service_->performKeywordSearch(request);
    if (const auto* error = std::get_if<ApiError>(&outcome)) {
      return errorReply(*error);
    }
    const SearchResponse& response = std::get<SearchResponse>(outcome);
    json reply = {
        {"results", resultsToJson(response.search.results)},
        {"expanded_terms", response.expansion.expandedTerms},
        {"expansion", expansionSourceName(response.expansion.source)},
        {"search_time_ms", response.search.timeMs},
        {"total_matches", response.search.totalMatches},
    };
    reply["time_ms"] = elapsedMs(started);
    return jsonReply(200, reply);
  } catch (...) {
    return internalErrorReply();
  }
}

HttpReply ApiServer::handleStats() {
  try {
    const ServiceStatus status = service_->status();
    return jsonReply(200,
                     {{"documents", status.stats.documents},
                      {"index_build_time_ms", status.stats.indexBuildTimeMs},
                      {"threads_used", status.stats.threadsUsed},
                      {"unique_terms", status.stats.uniqueTerms},
                      {"index_ready", status.indexReady},
                      {"indexing", status.indexing}});
  } catch (...) {
    return internalErrorReply();
  }
}

HttpReply ApiServer::handleStatic(const std::string& rawPath) {
  try {
    std::string path;
    if (!percentDecode(rawPath, path)) return notFoundPlain();
    if (path.empty() || path[0] != '/') return notFoundPlain();
    if (path.find('\0') != std::string::npos ||
        path.find('\\') != std::string::npos) {
      return notFoundPlain();
    }
    if (path == "/") path = "/index.html";

    // Reject any ".." segment before touching the file system.
    std::size_t start = 1;
    while (start <= path.size()) {
      const std::size_t end = path.find('/', start);
      const std::string segment = path.substr(
          start, end == std::string::npos ? std::string::npos : end - start);
      if (segment == "..") return notFoundPlain();
      if (end == std::string::npos) break;
      start = end + 1;
    }

    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::path(config_.webRoot), ec);
    if (ec) return notFoundPlain();
    const fs::path file = fs::weakly_canonical(root / path.substr(1), ec);
    if (ec) return notFoundPlain();
    // Defence in depth (symlinks): the resolved file must stay under the root.
    const auto relative = file.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") return notFoundPlain();
    if (!fs::is_regular_file(file, ec) || ec) return notFoundPlain();

    std::ifstream in(file, std::ios::binary);
    if (!in) return notFoundPlain();
    HttpReply reply;
    reply.contentType = contentTypeFor(file);
    reply.body.assign(std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>());
    return reply;
  } catch (...) {
    return HttpReply{500, "text/plain; charset=utf-8",
                     "500 Internal Server Error\n"};
  }
}

HttpReply ApiServer::handleRequest(const std::string& method,
                                   const std::string& path,
                                   const std::string& body) {
  if (startsWith(path, "/api/")) {
    struct Route {
      const char* path;
      const char* method;
    };
    static const Route kRoutes[] = {{"/api/health", "GET"},
                                    {"/api/index", "POST"},
                                    {"/api/search", "POST"},
                                    {"/api/stats", "GET"}};
    for (const Route& route : kRoutes) {
      if (path != route.path) continue;
      if (method != route.method) {
        return errorReply(ApiError{405, "method_not_allowed",
                                   "method " + method + " is not allowed on " +
                                       path + "; use " + route.method});
      }
      if (path == "/api/health") return handleHealthCheck();
      if (path == "/api/index") return handleIndexPost(body);
      if (path == "/api/search") return handleSearchPost(body);
      return handleStats();
    }
    return errorReply(ApiError{404, "not_found", "unknown API path: " + path});
  }
  if (startsWith(path, "/ws/")) return notFoundPlain();
  if (method != "GET" && method != "HEAD") {
    return HttpReply{405, "text/plain; charset=utf-8",
                     "405 Method Not Allowed\n"};
  }
  return handleStatic(path);
}

void ApiServer::handleWebSocket() {
  Impl* impl = impl_.get();
  SearchPipelineService* service = service_.get();
  CROW_WEBSOCKET_ROUTE(impl->app, "/ws/search")
      .max_payload(kCrowWebSocketLimit)
      .onopen([impl](crow::websocket::connection& conn) {
        auto session =
            std::make_shared<WebSocketSession>([&conn](std::string message) {
              conn.send_text(std::move(message));
            });
        std::lock_guard<std::mutex> lock(impl->sessionsMutex);
        impl->sessions[&conn] = std::move(session);
      })
      .onmessage([impl, service](crow::websocket::connection& conn,
                                 const std::string& data, bool isBinary) {
        std::shared_ptr<WebSocketSession> session;
        {
          std::lock_guard<std::mutex> lock(impl->sessionsMutex);
          const auto it = impl->sessions.find(&conn);
          if (it == impl->sessions.end()) return;
          session = it->second;
        }
        if (data.size() > kMaxWebSocketPayload) {
          session->close();  // drop queued work, no more output
          conn.close("message too big", 1009);
          return;
        }
        // Only queues the frame; the pipeline runs on the service's pool.
        service->handleWebSocketMessage(session, data, isBinary);
      })
      .onclose([impl](crow::websocket::connection& conn, const std::string&,
                      uint16_t) {
        std::shared_ptr<WebSocketSession> session;
        {
          std::lock_guard<std::mutex> lock(impl->sessionsMutex);
          const auto it = impl->sessions.find(&conn);
          if (it == impl->sessions.end()) return;
          session = std::move(it->second);
          impl->sessions.erase(it);
        }
        session->close();  // no send on this connection after this returns
      });
}

void ApiServer::run() {
  Impl* impl = impl_.get();
  impl->app.loglevel(crow::LogLevel::Warning);
  impl->app.signal_clear();
  handleWebSocket();

  CROW_CATCHALL_ROUTE(impl->app)
  ([this, impl](const crow::request& request) {
    const auto started = Clock::now();
    const std::string method = crow::method_name(request.method);
    HttpReply reply = handleRequest(method, request.url, request.body);
    crow::response response(reply.status, std::move(reply.body));
    response.set_header("Content-Type", reply.contentType);
    impl->logLine(method, request.url, reply.status, elapsedMs(started));
    return response;
  });

  impl->app.bindaddr("0.0.0.0")
      .port(static_cast<std::uint16_t>(config_.port))
      .concurrency(defaultWorkerThreads())
      .run();
}

void ApiServer::stop() { impl_->app.stop(); }

}  // namespace search
