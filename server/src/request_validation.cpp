#include "search/request_validation.h"

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <thread>

namespace search {

using nlohmann::json;

namespace {

ApiError makeError(int status, const char* code, const char* message) {
  return ApiError{status, code, message};
}

bool isAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

bool isBlank(std::string_view text) {
  for (char c : text) {
    if (!isAsciiSpace(c)) return false;
  }
  return true;
}

// JSON integer (signed or unsigned) -> int64, saturating for values above
// INT64_MAX. Floats, strings, booleans and null are not integers (§2.2).
bool readInteger(const json& value, std::int64_t& out) {
  if (value.is_boolean() || !value.is_number_integer()) return false;
  if (value.is_number_unsigned()) {
    const auto unsignedValue = value.get<std::uint64_t>();
    out = unsignedValue > static_cast<std::uint64_t>(
                              std::numeric_limits<std::int64_t>::max())
              ? std::numeric_limits<std::int64_t>::max()
              : static_cast<std::int64_t>(unsignedValue);
    return true;
  }
  out = value.get<std::int64_t>();
  return true;
}

// Returns the error for invalid JSON / non-object bodies, nullopt otherwise.
std::optional<ApiError> parseObject(std::string_view body, json& out) {
  // Invalid UTF-8 inside strings makes the parser fail -> invalid_json.
  out = json::parse(body.begin(), body.end(), nullptr, false);
  if (out.is_discarded()) {
    return makeError(400, "invalid_json", "request body is not valid JSON");
  }
  if (!out.is_object()) {
    return makeError(400, "invalid_body", "request body must be a JSON object");
  }
  return std::nullopt;
}

// A present, non-null member (null == absent, §2.1).
const json* member(const json& object, const char* name) {
  const auto it = object.find(name);
  if (it == object.end() || it->is_null()) return nullptr;
  return &*it;
}

}  // namespace

json restErrorJson(const ApiError& error) {
  return json{
      {"status", "error"}, {"code", error.code}, {"error", error.message}};
}

json webSocketErrorJson(const ApiError& error) {
  return json{{"stage", "error"},
              {"data", {{"code", error.code}, {"error", error.message}}}};
}

int defaultThreadCount() {
  const unsigned hardware = std::thread::hardware_concurrency();
  if (hardware == 0) return 1;
  return static_cast<int>(std::min<unsigned>(
      std::max<unsigned>(hardware, kMinThreads), kMaxThreads));
}

std::variant<IndexRequest, ApiError> parseIndexRequest(std::string_view body,
                                                       int defaultThreads) {
  IndexRequest request;
  request.threads = defaultThreads;
  if (isBlank(body)) return request;

  json object;
  if (auto error = parseObject(body, object)) return *error;

  if (const json* threads = member(object, "threads")) {
    std::int64_t value = 0;
    if (!readInteger(*threads, value) || value < kMinThreads ||
        value > kMaxThreads) {
      return makeError(400, "invalid_threads",
                       "threads must be an integer in [1, 64]");
    }
    request.threads = static_cast<int>(value);
  }
  return request;
}

std::variant<SearchRequest, ApiError> parseSearchRequest(std::string_view body,
                                                         MessageSource source) {
  if (isBlank(body)) {
    if (source == MessageSource::kWebSocketFrame) {
      return makeError(400, "invalid_json", "message is not valid JSON");
    }
    return makeError(400, "invalid_query",
                     "query is required (request body is empty)");
  }

  json object;
  if (auto error = parseObject(body, object)) {
    if (source == MessageSource::kWebSocketFrame &&
        error->code == "invalid_json") {
      error->message = "message is not valid JSON";
    }
    return *error;
  }

  SearchRequest request;

  // 1. query: invalid_query, then empty_query.
  const json* query = member(object, "query");
  if (query == nullptr || !query->is_string()) {
    return makeError(400, "invalid_query",
                     "query is required and must be a string");
  }
  const std::string& queryText = query->get_ref<const std::string&>();
  if (queryText.size() > kMaxQueryBytes) {
    return makeError(400, "invalid_query",
                     "query must not be longer than 1000 bytes");
  }
  if (isBlank(queryText)) {
    return makeError(400, "empty_query", "query must not be empty");
  }
  request.query = queryText;

  // 2. mode.
  if (const json* mode = member(object, "mode")) {
    if (!mode->is_string() ||
        mode->get_ref<const std::string&>() != "keyword") {
      return makeError(400, "invalid_mode", "mode must be \"keyword\"");
    }
  }

  // 3. limit.
  if (const json* limit = member(object, "limit")) {
    std::int64_t value = 0;
    if (!readInteger(*limit, value) || value < kMinLimit || value > kMaxLimit) {
      return makeError(400, "invalid_limit",
                       "limit must be an integer in [1, 100]");
    }
    request.limit = static_cast<int>(value);
  }

  // 4. expand.
  if (const json* expand = member(object, "expand")) {
    if (!expand->is_boolean()) {
      return makeError(400, "invalid_expand", "expand must be a boolean");
    }
    request.expand = expand->get<bool>();
  }
  return request;
}

}  // namespace search
