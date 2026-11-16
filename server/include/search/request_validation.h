// Request parsing and validation — docs/API.md §2, §3 (error model and
// validation order), §7.1, §8.1, §10.2. No sockets, no server state: pure
// functions that are unit-tested directly.
#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <variant>

namespace search {

inline constexpr std::size_t kMaxQueryBytes = 1000;
inline constexpr int kMinThreads = 1;
inline constexpr int kMaxThreads = 64;
inline constexpr int kMinLimit = 1;
inline constexpr int kMaxLimit = 100;
inline constexpr int kDefaultLimit = 10;

// HTTP status + machine-readable code (§3.2) + human-readable message.
struct ApiError {
  int httpStatus = 500;
  std::string code;
  std::string message;
};

// §3.1 error body: {"status":"error","code":...,"error":...}.
nlohmann::json restErrorJson(const ApiError& error);
// §10.3.4 WebSocket error message.
nlohmann::json webSocketErrorJson(const ApiError& error);

struct IndexRequest {
  int threads = 1;
};

struct SearchRequest {
  std::string query;  // exactly as received
  int limit = kDefaultLimit;
  bool expand = true;
};

// Default for `threads`: hardware_concurrency() clamped to [1, 64] (0 -> 1).
int defaultThreadCount();

// POST /api/index body. An empty / whitespace-only body means "{}".
// `defaultThreads` is used when `threads` is absent or null.
std::variant<IndexRequest, ApiError> parseIndexRequest(std::string_view body,
                                                       int defaultThreads);

// POST /api/search body and WebSocket text frame (same rules, §10.2).
// An empty / whitespace-only body: REST -> invalid_query (§2.1);
// WebSocket frame -> invalid_json (the frame is not valid JSON, §10.5).
enum class MessageSource { kHttpBody, kWebSocketFrame };
std::variant<SearchRequest, ApiError> parseSearchRequest(
    std::string_view body, MessageSource source = MessageSource::kHttpBody);

}  // namespace search
