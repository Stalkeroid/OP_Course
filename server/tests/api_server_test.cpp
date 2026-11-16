#include "search/api_server.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

#include "test_util.h"

using nlohmann::json;
using search::ApiServer;
using search::Config;
using search::HttpReply;
using search::SearchPipelineService;
using search_test::TempDir;

namespace {

struct Fixture {
  TempDir dataset;
  TempDir web;
  Config config;
  std::shared_ptr<SearchPipelineService> service;
  std::unique_ptr<ApiServer> server;

  Fixture() {
    dataset.write("a.txt", "Climate change effects on coastal cities.");
    dataset.write("b.txt", "Global warming raises sea levels.");
    config.datasetPath = dataset.path().string();
    config.webRoot = web.path().string();
    config.ollamaUrl = "http://127.0.0.1:1";  // refused -> fallback
    config.ollamaTimeoutMs = 500;
    service = std::make_shared<SearchPipelineService>(config);
    server = std::make_unique<ApiServer>(config, service);
  }
  HttpReply get(const std::string& path) {
    return server->handleRequest("GET", path, "");
  }
  HttpReply post(const std::string& path, const std::string& body) {
    return server->handleRequest("POST", path, body);
  }
};

void expectError(const HttpReply& reply, int status, const std::string& code) {
  EXPECT_EQ(reply.status, status) << reply.body;
  EXPECT_EQ(reply.contentType, "application/json");
  const json body = json::parse(reply.body);
  ASSERT_TRUE(body.is_object());
  EXPECT_EQ(body.size(), 3u);
  EXPECT_EQ(body["status"], "error");
  EXPECT_EQ(body["code"], code);
  EXPECT_TRUE(body["error"].is_string());
  EXPECT_FALSE(std::string(body["error"]).empty());
}

}  // namespace

TEST(ApiServerRoutes, HealthShape) {
  Fixture f;
  const HttpReply reply = f.get("/api/health");
  EXPECT_EQ(reply.status, 200);
  EXPECT_EQ(reply.contentType, "application/json");
  const json body = json::parse(reply.body);
  EXPECT_EQ(body, (json{{"status", "ok"},
                        {"ollama", false},
                        {"index_ready", false},
                        {"indexing", false}}));
}

TEST(ApiServerRoutes, StatsBeforeIndexAreZero) {
  Fixture f;
  const json body = json::parse(f.get("/api/stats").body);
  EXPECT_EQ(body, (json{{"documents", 0},
                        {"index_build_time_ms", 0},
                        {"threads_used", 0},
                        {"unique_terms", 0},
                        {"index_ready", false},
                        {"indexing", false}}));
}

TEST(ApiServerRoutes, SearchBeforeIndexIs503ButValidationComesFirst) {
  Fixture f;
  expectError(f.post("/api/search", R"({"query":"test","mode":"keyword"})"),
              503, "index_not_built");
  expectError(f.post("/api/search", R"({"query":"test","limit":0})"), 400,
              "invalid_limit");
}

TEST(ApiServerRoutes, IndexSearchStatsRoundTrip) {
  Fixture f;
  const HttpReply built = f.post("/api/index", R"({"threads":2})");
  ASSERT_EQ(built.status, 200) << built.body;
  const json b = json::parse(built.body);
  EXPECT_EQ(b["status"], "ok");
  EXPECT_EQ(b["documents_indexed"], 2);
  EXPECT_TRUE(b["time_ms"].is_number_integer());
  EXPECT_EQ(b["threads_used"], 2);
  EXPECT_EQ(b["files_skipped"], 0);
  EXPECT_EQ(b.size(), 5u);

  const json stats = json::parse(f.get("/api/stats").body);
  EXPECT_EQ(stats["documents"], 2);
  EXPECT_EQ(stats["index_build_time_ms"], b["time_ms"]);
  EXPECT_EQ(stats["threads_used"], 2);
  EXPECT_TRUE(stats["index_ready"].get<bool>());
  EXPECT_FALSE(stats["indexing"].get<bool>());

  const HttpReply searched =
      f.post("/api/search", R"({"query":"climate change","limit":5})");
  ASSERT_EQ(searched.status, 200) << searched.body;
  EXPECT_EQ(searched.contentType, "application/json");
  const json s = json::parse(searched.body);
  EXPECT_EQ(s.size(), 6u);
  ASSERT_EQ(s["results"].size(), 1u);
  EXPECT_EQ(s["results"][0]["doc_id"], "a.txt");
  EXPECT_EQ(s["results"][0]["score"], 1.0);
  EXPECT_EQ(s["results"][0].size(), 3u);
  EXPECT_TRUE(s["time_ms"].is_number_integer());
  EXPECT_TRUE(s["search_time_ms"].is_number_integer());
  EXPECT_EQ(s["total_matches"], 1);
  EXPECT_EQ(s["expansion"], "fallback");
  EXPECT_EQ(s["expanded_terms"], (json::array({"climate", "change"})));

  const json disabled = json::parse(
      f.post("/api/search", R"({"query":"climate","expand":false})").body);
  EXPECT_EQ(disabled["expansion"], "disabled");
  const json none =
      json::parse(f.post("/api/search", R"({"query":"the of"})").body);
  EXPECT_EQ(none["expansion"], "disabled");
  EXPECT_TRUE(none["results"].empty());
  EXPECT_TRUE(none["expanded_terms"].empty());
}

TEST(ApiServerRoutes, IndexValidationAndDatasetErrors) {
  Fixture f;
  for (const char* body :
       {R"({"threads":0})", R"({"threads":65})", R"({"threads":"4"})",
        R"({"threads":4.5})", R"({"threads":true})"}) {
    expectError(f.post("/api/index", body), 400, "invalid_threads");
  }
  expectError(f.post("/api/index", "{bad json"), 400, "invalid_json");
  expectError(f.post("/api/index", "[1]"), 400, "invalid_body");
  // Empty body and {} are accepted (default threads).
  EXPECT_EQ(f.post("/api/index", "").status, 200);
  EXPECT_EQ(f.post("/api/index", "{}").status, 200);

  Fixture g;
  g.config.datasetPath = (g.dataset.path() / "missing").string();
  g.service = std::make_shared<SearchPipelineService>(g.config);
  g.server = std::make_unique<ApiServer>(g.config, g.service);
  expectError(g.post("/api/index", "{}"), 500, "dataset_not_found");
}

TEST(ApiServerRoutes, UnknownApiPathAndWrongMethod) {
  Fixture f;
  expectError(f.get("/api/unknown"), 404, "not_found");
  expectError(f.get("/api/"), 404, "not_found");
  expectError(f.get("/api/health/"), 404, "not_found");
  expectError(f.post("/api/unknown", "{}"), 404, "not_found");
  expectError(f.get("/api/search"), 405, "method_not_allowed");
  expectError(f.get("/api/index"), 405, "method_not_allowed");
  expectError(f.post("/api/health", "{}"), 405, "method_not_allowed");
  expectError(f.post("/api/stats", "{}"), 405, "method_not_allowed");
  expectError(f.server->handleRequest("DELETE", "/api/search", ""), 405,
              "method_not_allowed");
}

TEST(ApiServerStatic, ServesIndexAndFilesWithContentTypes) {
  Fixture f;
  f.web.write("index.html", "<html>hi</html>");
  f.web.write("app.js", "console.log(1)");
  f.web.write("style.css", "body{}");
  f.web.write("logo.svg", "<svg/>");
  f.web.write("data.json", "{}");
  f.web.write("favicon.ico", "ico");
  f.web.write("img/pic.png", "png");

  HttpReply root = f.get("/");
  EXPECT_EQ(root.status, 200);
  EXPECT_EQ(root.body, "<html>hi</html>");
  EXPECT_NE(root.contentType.find("text/html"), std::string::npos);
  EXPECT_NE(f.get("/app.js").contentType.find("javascript"), std::string::npos);
  EXPECT_NE(f.get("/style.css").contentType.find("text/css"),
            std::string::npos);
  EXPECT_EQ(f.get("/logo.svg").contentType, "image/svg+xml");
  EXPECT_EQ(f.get("/data.json").contentType, "application/json");
  EXPECT_EQ(f.get("/favicon.ico").contentType, "image/x-icon");
  EXPECT_EQ(f.get("/img/pic.png").contentType, "image/png");
  EXPECT_EQ(f.get("/index.html").status, 200);
}

TEST(ApiServerStatic, MissingFileIsPlain404) {
  Fixture f;
  f.web.write("index.html", "x");
  const HttpReply reply = f.get("/nope.js");
  EXPECT_EQ(reply.status, 404);
  EXPECT_NE(reply.contentType.find("text/plain"), std::string::npos);
  EXPECT_EQ(f.get("/img").status, 404);  // not a regular file
  Fixture empty;
  EXPECT_EQ(empty.get("/").status, 404);  // no index.html
}

TEST(ApiServerStatic, PathTraversalIsRejected) {
  Fixture f;
  f.web.write("index.html", "x");
  // A secret outside the web root (sibling directory).
  f.dataset.write("secret.txt", "TOP SECRET");
  const std::string rel =
      "/../" + f.dataset.path().filename().string() + "/secret.txt";
  for (const std::string& path :
       {rel, std::string("/../../etc/passwd"),
        std::string("/%2e%2e/%2e%2e/etc/passwd"),
        std::string("/..%2f..%2fetc/passwd"),
        std::string("/a/../../etc/passwd"), std::string("/..\\..\\etc\\passwd"),
        std::string("/%00"), std::string("/%zz")}) {
    const HttpReply reply = f.get(path);
    EXPECT_EQ(reply.status, 404) << path;
    EXPECT_EQ(reply.body.find("SECRET"), std::string::npos) << path;
    EXPECT_EQ(reply.body.find("root:"), std::string::npos) << path;
  }
}

TEST(ApiServerStatic, SymlinkOutOfRootIsRejected) {
  Fixture f;
  f.dataset.write("secret.txt", "TOP SECRET");
  std::filesystem::create_symlink(f.dataset.path() / "secret.txt",
                                  f.web.path() / "link.txt");
  EXPECT_EQ(f.get("/link.txt").status, 404);
}

TEST(ApiServerStatic, NonGetOnStaticPathIs405AndWsPathIs404) {
  Fixture f;
  f.web.write("index.html", "x");
  EXPECT_EQ(f.post("/", "").status, 405);
  EXPECT_EQ(f.get("/ws/other").status, 404);
}
