#include "search/request_validation.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>
#include <variant>

using search::ApiError;
using search::IndexRequest;
using search::MessageSource;
using search::SearchRequest;

namespace {

// Returns the error code, or "" when the request is valid.
std::string searchCode(const std::string& body,
                       MessageSource source = MessageSource::kHttpBody) {
  auto result = search::parseSearchRequest(body, source);
  if (const auto* error = std::get_if<ApiError>(&result)) return error->code;
  return "";
}

std::string indexCode(const std::string& body) {
  auto result = search::parseIndexRequest(body, 4);
  if (const auto* error = std::get_if<ApiError>(&result)) return error->code;
  return "";
}

SearchRequest okSearch(const std::string& body) {
  auto result = search::parseSearchRequest(body);
  EXPECT_TRUE(std::holds_alternative<SearchRequest>(result)) << body;
  return std::get<SearchRequest>(result);
}

}  // namespace

TEST(IndexValidation, EmptyAndWhitespaceBodyMeansDefaults) {
  for (const char* body : {"", "   ", "\n\t "}) {
    auto result = search::parseIndexRequest(body, 7);
    ASSERT_TRUE(std::holds_alternative<IndexRequest>(result)) << body;
    EXPECT_EQ(std::get<IndexRequest>(result).threads, 7);
  }
}

TEST(IndexValidation, ThreadsDefaultsForEmptyObjectAndNull) {
  for (const char* body : {"{}", "{\"threads\":null}", "{\"x\":1}"}) {
    auto result = search::parseIndexRequest(body, 5);
    ASSERT_TRUE(std::holds_alternative<IndexRequest>(result)) << body;
    EXPECT_EQ(std::get<IndexRequest>(result).threads, 5);
  }
}

TEST(IndexValidation, ThreadsBoundaries) {
  auto one = search::parseIndexRequest("{\"threads\":1}", 4);
  auto max = search::parseIndexRequest("{\"threads\":64}", 4);
  EXPECT_EQ(std::get<IndexRequest>(one).threads, 1);
  EXPECT_EQ(std::get<IndexRequest>(max).threads, 64);
  EXPECT_EQ(indexCode("{\"threads\":0}"), "invalid_threads");
  EXPECT_EQ(indexCode("{\"threads\":65}"), "invalid_threads");
  EXPECT_EQ(indexCode("{\"threads\":-3}"), "invalid_threads");
  EXPECT_EQ(indexCode("{\"threads\":18446744073709551615}"), "invalid_threads");
}

TEST(IndexValidation, ThreadsWrongTypes) {
  for (const char* body :
       {"{\"threads\":4.5}", "{\"threads\":4.0}", "{\"threads\":\"4\"}",
        "{\"threads\":true}", "{\"threads\":false}", "{\"threads\":[4]}",
        "{\"threads\":{}}", "{\"threads\":4e0}"}) {
    EXPECT_EQ(indexCode(body), "invalid_threads") << body;
  }
}

TEST(IndexValidation, BodyErrors) {
  EXPECT_EQ(indexCode("{bad json"), "invalid_json");
  EXPECT_EQ(indexCode("{\"threads\":4} trailing"), "invalid_json");
  EXPECT_EQ(indexCode("[1]"), "invalid_body");
  EXPECT_EQ(indexCode("\"x\""), "invalid_body");
  EXPECT_EQ(indexCode("5"), "invalid_body");
  EXPECT_EQ(indexCode("null"), "invalid_body");
}

TEST(SearchValidation, MinimalRequestUsesDefaults) {
  SearchRequest request = okSearch("{\"query\":\"climate change\"}");
  EXPECT_EQ(request.query, "climate change");
  EXPECT_EQ(request.limit, 10);
  EXPECT_TRUE(request.expand);
}

TEST(SearchValidation, AllFieldsAndUnknownFieldsIgnored) {
  SearchRequest request = okSearch(
      "{\"query\":\"  Hello  \",\"mode\":\"keyword\",\"limit\":100,"
      "\"expand\":false,\"extra\":[1,2]}");
  EXPECT_EQ(request.query, "  Hello  ");  // kept exactly as received
  EXPECT_EQ(request.limit, 100);
  EXPECT_FALSE(request.expand);
}

TEST(SearchValidation, NullFieldsAreAbsent) {
  SearchRequest request = okSearch(
      "{\"query\":\"a\",\"mode\":null,\"limit\":null,\"expand\":null}");
  EXPECT_EQ(request.limit, 10);
  EXPECT_TRUE(request.expand);
}

TEST(SearchValidation, EmptyBodyIsInvalidQueryForRestInvalidJsonForWebSocket) {
  EXPECT_EQ(searchCode(""), "invalid_query");
  EXPECT_EQ(searchCode("  \n"), "invalid_query");
  EXPECT_EQ(searchCode("", MessageSource::kWebSocketFrame), "invalid_json");
}

TEST(SearchValidation, BodyErrors) {
  EXPECT_EQ(searchCode("not json"), "invalid_json");
  EXPECT_EQ(searchCode("{\"query\":\"a\""), "invalid_json");
  EXPECT_EQ(searchCode("[]"), "invalid_body");
  EXPECT_EQ(searchCode("\"x\""), "invalid_body");
  EXPECT_EQ(searchCode("5"), "invalid_body");
  EXPECT_EQ(searchCode("not json", MessageSource::kWebSocketFrame),
            "invalid_json");
  EXPECT_EQ(searchCode("[]", MessageSource::kWebSocketFrame), "invalid_body");
}

TEST(SearchValidation, InvalidUtf8IsInvalidJson) {
  EXPECT_EQ(searchCode(std::string("{\"query\":\"a\xff\xfe\"}")),
            "invalid_json");
}

TEST(SearchValidation, InvalidQuery) {
  EXPECT_EQ(searchCode("{}"), "invalid_query");
  EXPECT_EQ(searchCode("{\"query\":null}"), "invalid_query");
  EXPECT_EQ(searchCode("{\"query\":5}"), "invalid_query");
  EXPECT_EQ(searchCode("{\"query\":true}"), "invalid_query");
  EXPECT_EQ(searchCode("{\"query\":[\"a\"]}"), "invalid_query");
  const std::string tooLong(1001, 'a');
  EXPECT_EQ(searchCode("{\"query\":\"" + tooLong + "\"}"), "invalid_query");
  const std::string exact(1000, 'a');
  EXPECT_EQ(searchCode("{\"query\":\"" + exact + "\"}"), "");
}

TEST(SearchValidation, QueryLengthIsMeasuredInBytes) {
  // 501 two-byte characters = 1002 bytes -> invalid_query.
  std::string text;
  for (int i = 0; i < 501; ++i) text += "\xc3\xa9";
  EXPECT_EQ(searchCode("{\"query\":\"" + text + "\"}"), "invalid_query");
  std::string fits;
  for (int i = 0; i < 500; ++i) fits += "\xc3\xa9";
  EXPECT_EQ(searchCode("{\"query\":\"" + fits + "\"}"), "");
}

TEST(SearchValidation, EmptyQuery) {
  EXPECT_EQ(searchCode("{\"query\":\"\"}"), "empty_query");
  EXPECT_EQ(searchCode("{\"query\":\"   \"}"), "empty_query");
  EXPECT_EQ(searchCode("{\"query\":\" \\t\\n\\r\"}"), "empty_query");
  // Only stop words / punctuation is NOT empty_query (§4.2).
  EXPECT_EQ(searchCode("{\"query\":\"the of !!\"}"), "");
}

TEST(SearchValidation, Mode) {
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"mode\":\"semantic\"}"),
            "invalid_mode");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"mode\":\"Keyword\"}"),
            "invalid_mode");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"mode\":5}"), "invalid_mode");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"mode\":true}"), "invalid_mode");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"mode\":\"keyword\"}"), "");
}

TEST(SearchValidation, Limit) {
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"limit\":0}"), "invalid_limit");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"limit\":101}"), "invalid_limit");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"limit\":-1}"), "invalid_limit");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"limit\":2.5}"), "invalid_limit");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"limit\":10.0}"), "invalid_limit");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"limit\":\"5\"}"), "invalid_limit");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"limit\":true}"), "invalid_limit");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"limit\":1}"), "");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"limit\":100}"), "");
}

TEST(SearchValidation, Expand) {
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"expand\":\"no\"}"),
            "invalid_expand");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"expand\":0}"), "invalid_expand");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"expand\":[]}"), "invalid_expand");
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"expand\":true}"), "");
}

TEST(SearchValidation, ValidationOrderStopsAtFirstFailure) {
  // query before mode/limit/expand.
  EXPECT_EQ(searchCode("{\"mode\":\"x\",\"limit\":0,\"expand\":1}"),
            "invalid_query");
  EXPECT_EQ(searchCode("{\"query\":\"\",\"mode\":\"x\"}"), "empty_query");
  // invalid_query before empty_query is moot, but mode before limit:
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"mode\":\"x\",\"limit\":0}"),
            "invalid_mode");
  // limit before expand.
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"limit\":0,\"expand\":1}"),
            "invalid_limit");
  // Validation (400) does not depend on server state (§3.3).
  EXPECT_EQ(searchCode("{\"query\":\"a\",\"expand\":1}"), "invalid_expand");
}

TEST(SearchValidation, ErrorsCarryHttpStatusAndMessage) {
  auto result = search::parseSearchRequest("{\"query\":5}");
  const auto& error = std::get<ApiError>(result);
  EXPECT_EQ(error.httpStatus, 400);
  EXPECT_FALSE(error.message.empty());
}

TEST(ErrorJson, RestAndWebSocketShapes) {
  ApiError error{409, "index_in_progress", "busy"};
  EXPECT_EQ(search::restErrorJson(error),
            (nlohmann::json{{"status", "error"},
                            {"code", "index_in_progress"},
                            {"error", "busy"}}));
  EXPECT_EQ(search::webSocketErrorJson(error),
            (nlohmann::json{
                {"stage", "error"},
                {"data", {{"code", "index_in_progress"}, {"error", "busy"}}}}));
}

TEST(DefaultThreads, InRange) {
  const int threads = search::defaultThreadCount();
  EXPECT_GE(threads, 1);
  EXPECT_LE(threads, 64);
}
