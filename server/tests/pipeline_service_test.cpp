#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "fake_ollama.h"
#include "search/search_pipeline_service.h"
#include "test_util.h"

using nlohmann::json;
using search::ApiError;
using search::Config;
using search::PipelineOptions;
using search::SearchPipelineService;
using search::SearchRequest;
using search::SearchResponse;
using search::WebSocketSession;
using search_test::FakeOllama;
using search_test::TempDir;

namespace {

void writeDataset(const TempDir& dir) {
  dir.write("a.txt", "Climate change effects on coastal cities and flooding.");
  dir.write("b.txt", "Global warming raises sea levels around the world.");
  dir.write("c.txt", "Cooking recipes: pasta with tomato sauce.");
  dir.write("sub/d.txt", "climate climate climate everywhere");
}

Config configFor(const TempDir& dir, const std::string& ollamaUrl,
                 int timeoutMs = 1000) {
  Config config;
  config.datasetPath = dir.path().string();
  config.ollamaUrl = ollamaUrl;
  config.ollamaTimeoutMs = timeoutMs;
  return config;
}

std::string deadUrl() { return "http://127.0.0.1:1"; }  // refused

SearchRequest request(const std::string& query, bool expand = true,
                      int limit = 10) {
  SearchRequest r;
  r.query = query;
  r.expand = expand;
  r.limit = limit;
  return r;
}

// Collects the messages of one WebSocket session.
class Collector {
 public:
  std::shared_ptr<WebSocketSession> makeSession() {
    return std::make_shared<WebSocketSession>([this](std::string message) {
      std::lock_guard<std::mutex> lock(mutex_);
      messages_.push_back(json::parse(message));
      cv_.notify_all();
    });
  }
  bool waitFor(std::size_t count, int ms = 10000) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(ms),
                        [&] { return messages_.size() >= count; });
  }
  std::vector<json> messages() {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_;
  }
  std::vector<std::string> stages() {
    std::vector<std::string> out;
    for (const auto& m : messages()) out.push_back(m["stage"]);
    return out;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<json> messages_;
};

}  // namespace

TEST(PipelineService, SearchBeforeIndexIs503) {
  TempDir dir;
  writeDataset(dir);
  SearchPipelineService service(configFor(dir, deadUrl()));
  auto outcome = service.performKeywordSearch(request("climate"));
  ASSERT_TRUE(std::holds_alternative<ApiError>(outcome));
  EXPECT_EQ(std::get<ApiError>(outcome).code, "index_not_built");
  EXPECT_EQ(std::get<ApiError>(outcome).httpStatus, 503);
  const auto status = service.status();
  EXPECT_FALSE(status.indexReady);
  EXPECT_FALSE(status.indexing);
  EXPECT_EQ(status.stats.documents, 0u);
  EXPECT_EQ(status.stats.threadsUsed, 0);
  EXPECT_EQ(status.stats.indexBuildTimeMs, 0);
}

TEST(PipelineService, BuildAndSearchWithFallback) {
  TempDir dir;
  writeDataset(dir);
  SearchPipelineService service(configFor(dir, deadUrl()));
  const auto built = service.buildIndex(2);
  ASSERT_FALSE(built.error.has_value());
  EXPECT_EQ(built.stats.documents, 4u);
  EXPECT_EQ(built.stats.threadsUsed, 2);

  auto outcome = service.performKeywordSearch(request("Climate change"));
  ASSERT_TRUE(std::holds_alternative<SearchResponse>(outcome));
  const auto& response = std::get<SearchResponse>(outcome);
  EXPECT_EQ(response.expansion.source, search::ExpansionSource::kFallback);
  EXPECT_EQ(response.expansion.expandedTerms,
            (std::vector<std::string>{"climate", "change"}));
  ASSERT_EQ(response.search.results.size(), 2u);
  EXPECT_EQ(response.search.totalMatches, 2u);
  EXPECT_DOUBLE_EQ(response.search.results[0].score, 1.0);
  for (const auto& r : response.search.results) {
    EXPECT_FALSE(r.snippet.empty());
  }
  const auto status = service.status();
  EXPECT_TRUE(status.indexReady);
  EXPECT_EQ(status.stats.documents, 4u);
  EXPECT_EQ(status.stats.threadsUsed, 2);
}

TEST(PipelineService, LlmExpansionAddsTermsWithOriginalsFirst) {
  TempDir dir;
  writeDataset(dir);
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["Global Warming","climate","sea"]})");
  SearchPipelineService service(configFor(dir, fake.url()));
  ASSERT_FALSE(service.buildIndex(1).error.has_value());
  auto outcome = service.performKeywordSearch(request("climate change"));
  const auto& response = std::get<SearchResponse>(outcome);
  EXPECT_EQ(response.expansion.source, search::ExpansionSource::kLlm);
  EXPECT_EQ(response.expansion.expandedTerms,
            (std::vector<std::string>{"climate", "change", "global", "warming",
                                      "sea"}));
  EXPECT_EQ(fake.generateCalls(), 1);
  // b.txt matches only through expansion terms.
  bool foundB = false;
  for (const auto& r : response.search.results) foundB |= r.docId == "b.txt";
  EXPECT_TRUE(foundB);
}

TEST(PipelineService, ExpandFalseAndNoTokenQueryDoNotCallOllama) {
  TempDir dir;
  writeDataset(dir);
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["x1"]})");
  SearchPipelineService service(configFor(dir, fake.url()));
  ASSERT_FALSE(service.buildIndex(1).error.has_value());

  auto off = std::get<SearchResponse>(
      service.performKeywordSearch(request("climate", false)));
  EXPECT_EQ(off.expansion.source, search::ExpansionSource::kDisabled);
  EXPECT_EQ(off.expansion.expandedTerms, (std::vector<std::string>{"climate"}));

  auto none = std::get<SearchResponse>(
      service.performKeywordSearch(request("the of !!")));
  EXPECT_EQ(none.expansion.source, search::ExpansionSource::kDisabled);
  EXPECT_TRUE(none.expansion.expandedTerms.empty());
  EXPECT_TRUE(none.search.results.empty());
  EXPECT_EQ(none.search.totalMatches, 0u);
  EXPECT_EQ(fake.generateCalls(), 0);
}

TEST(PipelineService, NoMatchAndLimit) {
  TempDir dir;
  writeDataset(dir);
  SearchPipelineService service(configFor(dir, deadUrl()));
  ASSERT_FALSE(service.buildIndex(1).error.has_value());
  auto none = std::get<SearchResponse>(
      service.performKeywordSearch(request("zzqxjv", false)));
  EXPECT_TRUE(none.search.results.empty());
  auto limited = std::get<SearchResponse>(
      service.performKeywordSearch(request("climate", false, 1)));
  EXPECT_EQ(limited.search.results.size(), 1u);
  EXPECT_EQ(limited.search.totalMatches, 2u);
  EXPECT_EQ(limited.search.results[0].docId, "sub/d.txt");
}

TEST(PipelineService, DatasetErrorsKeepOldIndex) {
  TempDir dir;
  writeDataset(dir);
  TempDir emptyDir;
  Config config = configFor(dir, deadUrl());
  SearchPipelineService service(config);
  ASSERT_FALSE(service.buildIndex(1).error.has_value());

  // Point a second service at a missing / empty dataset.
  Config missing = configFor(dir, deadUrl());
  missing.datasetPath = (dir.path() / "nope").string();
  SearchPipelineService other(missing);
  auto error = other.buildIndex(1).error;
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "dataset_not_found");
  EXPECT_EQ(error->httpStatus, 500);

  Config empty = configFor(emptyDir, deadUrl());
  SearchPipelineService third(empty);
  error = third.buildIndex(1).error;
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "dataset_empty");
  EXPECT_FALSE(third.status().indexReady);

  // Failure of a rebuild keeps the previous index: swap the dataset away.
  std::filesystem::rename(dir.path() / "a.txt", dir.path() / "a.bin");
  std::filesystem::rename(dir.path() / "b.txt", dir.path() / "b.bin");
  std::filesystem::rename(dir.path() / "c.txt", dir.path() / "c.bin");
  std::filesystem::remove_all(dir.path() / "sub");
  error = service.buildIndex(1).error;
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "dataset_empty");
  EXPECT_TRUE(service.status().indexReady);
  EXPECT_EQ(service.status().stats.documents, 4u);
}

TEST(PipelineService, BuilderExceptionIsInternalError) {
  TempDir dir;
  PipelineOptions options;
  options.builder = [](const std::filesystem::path&,
                       int) -> search::BuildOutcome {
    throw std::runtime_error("boom");
  };
  SearchPipelineService service(configFor(dir, deadUrl()), options);
  auto error = service.buildIndex(1).error;
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "internal_error");
  EXPECT_EQ(error->httpStatus, 500);
  // The flag is released: a second attempt is not "in progress".
  EXPECT_EQ(service.buildIndex(1).error->code, "internal_error");
}

TEST(PipelineService, SearchWorksDuringRebuildAndSwapHappens) {
  TempDir dir;
  writeDataset(dir);
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> releaseFuture = release.get_future().share();
  std::atomic<int> calls{0};

  PipelineOptions options;
  options.builder = [&](const std::filesystem::path& path, int threads) {
    if (calls.fetch_add(1) == 1) {  // only the second build is slow
      entered.set_value();
      releaseFuture.wait();
    }
    return search::IndexBuilder::buildIndex(path, threads);
  };
  SearchPipelineService service(configFor(dir, deadUrl()), options);
  ASSERT_FALSE(service.buildIndex(1).error.has_value());  // old index: 4 docs

  dir.write("e.txt", "climate newcomer document");  // visible only after swap
  std::thread rebuild([&] {
    auto result = service.buildIndex(2);
    EXPECT_FALSE(result.error.has_value());
    EXPECT_EQ(result.stats.documents, 5u);
  });
  entered.get_future().wait();

  // Rebuild is running: old index still serves searches and stats.
  EXPECT_TRUE(service.status().indexing);
  EXPECT_TRUE(service.status().indexReady);
  EXPECT_EQ(service.status().stats.documents, 4u);
  auto during = std::get<SearchResponse>(
      service.performKeywordSearch(request("climate", false)));
  EXPECT_EQ(during.search.totalMatches, 2u);

  // Second concurrent build -> 409 (the 409 equivalent at service level).
  auto second = service.buildIndex(1);
  ASSERT_TRUE(second.error.has_value());
  EXPECT_EQ(second.error->code, "index_in_progress");
  EXPECT_EQ(second.error->httpStatus, 409);

  release.set_value();
  rebuild.join();

  EXPECT_FALSE(service.status().indexing);
  EXPECT_EQ(service.status().stats.documents, 5u);
  EXPECT_EQ(service.status().stats.threadsUsed, 2);
  auto after = std::get<SearchResponse>(
      service.performKeywordSearch(request("climate", false)));
  EXPECT_EQ(after.search.totalMatches, 3u);
}

TEST(PipelineService, ConcurrentSearchesAndRebuildsAreSafe) {
  TempDir dir;
  writeDataset(dir);
  SearchPipelineService service(configFor(dir, deadUrl(), 200));
  ASSERT_FALSE(service.buildIndex(2).error.has_value());
  std::atomic<bool> stop{false};
  std::vector<std::thread> searchers;
  std::atomic<int> failures{0};
  for (int i = 0; i < 4; ++i) {
    searchers.emplace_back([&] {
      while (!stop.load()) {
        auto outcome = service.performKeywordSearch(request("climate", false));
        if (!std::holds_alternative<SearchResponse>(outcome)) failures++;
      }
    });
  }
  for (int i = 0; i < 5; ++i) {
    EXPECT_FALSE(service.buildIndex(1 + i % 3).error.has_value());
  }
  stop = true;
  for (auto& t : searchers) t.join();
  EXPECT_EQ(failures.load(), 0);
}

// --- WebSocket pipeline ------------------------------------------------------

TEST(PipelineWebSocket, ThreeMessageSequenceThenSecondQuery) {
  TempDir dir;
  writeDataset(dir);
  SearchPipelineService service(configFor(dir, deadUrl()),
                                {nullptr, nullptr, 4});
  ASSERT_FALSE(service.buildIndex(1).error.has_value());
  Collector collector;
  auto session = collector.makeSession();

  service.handleWebSocketMessage(session, R"({"query":"Climate change"})",
                                 false);
  ASSERT_TRUE(collector.waitFor(3));
  auto messages = collector.messages();
  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[0]["stage"], "query_expansion");
  EXPECT_EQ(messages[0]["data"]["original"], "Climate change");
  EXPECT_EQ(messages[0]["data"]["expansion"], "fallback");
  EXPECT_EQ(messages[0]["data"]["expanded_terms"],
            (json::array({"climate", "change"})));
  EXPECT_TRUE(messages[0]["data"]["time_ms"].is_number_integer());
  EXPECT_EQ(messages[1]["stage"], "keyword_search");
  EXPECT_EQ(messages[1]["data"]["total_matches"], 2);
  EXPECT_TRUE(messages[1]["data"]["time_ms"].is_number_integer());
  EXPECT_EQ(messages[1]["data"]["results"][0]["score"], 1.0);
  EXPECT_EQ(messages[2], (json{{"stage", "done"}}));

  service.handleWebSocketMessage(session, R"({"query":"pasta","expand":false})",
                                 false);
  ASSERT_TRUE(collector.waitFor(6));
  EXPECT_EQ(
      collector.stages(),
      (std::vector<std::string>{"query_expansion", "keyword_search", "done",
                                "query_expansion", "keyword_search", "done"}));
}

TEST(PipelineWebSocket, ErrorsAreSingleMessagesAndConnectionStaysUsable) {
  TempDir dir;
  writeDataset(dir);
  SearchPipelineService service(configFor(dir, deadUrl()),
                                {nullptr, nullptr, 2});
  ASSERT_FALSE(service.buildIndex(1).error.has_value());
  Collector collector;
  auto session = collector.makeSession();

  service.handleWebSocketMessage(session, "not json", false);
  service.handleWebSocketMessage(session, "[1]", false);
  service.handleWebSocketMessage(session, R"({"query":""})", false);
  service.handleWebSocketMessage(session, R"({"query":"a","limit":0})", false);
  service.handleWebSocketMessage(session, "\x01\x02", true);
  service.handleWebSocketMessage(session, R"({"query":"pasta","expand":false})",
                                 false);
  ASSERT_TRUE(collector.waitFor(8));
  const auto messages = collector.messages();
  ASSERT_EQ(messages.size(), 8u);
  const char* codes[] = {"invalid_json", "invalid_body", "empty_query",
                         "invalid_limit", "invalid_json"};
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(messages[i]["stage"], "error");
    EXPECT_EQ(messages[i]["data"]["code"], codes[i]);
    EXPECT_FALSE(std::string(messages[i]["data"]["error"]).empty());
  }
  EXPECT_EQ(messages[5]["stage"], "query_expansion");
  EXPECT_EQ(messages[7], (json{{"stage", "done"}}));
}

TEST(PipelineWebSocket, IndexNotBuiltIsSingleErrorWithoutExpansion) {
  TempDir dir;
  writeDataset(dir);
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["x1"]})");
  SearchPipelineService service(configFor(dir, fake.url()),
                                {nullptr, nullptr, 2});
  Collector collector;
  auto session = collector.makeSession();
  service.handleWebSocketMessage(session, R"({"query":"test"})", false);
  ASSERT_TRUE(collector.waitFor(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto messages = collector.messages();
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0]["data"]["code"], "index_not_built");
  EXPECT_EQ(fake.generateCalls(), 0);
}

TEST(PipelineWebSocket, QueryExpansionIsSentBeforeSearchStarts) {
  TempDir dir;
  writeDataset(dir);
  SearchPipelineService service(configFor(dir, deadUrl()));
  ASSERT_FALSE(service.buildIndex(1).error.has_value());
  std::vector<std::string> stages;
  service.performStreamingSearch(request("climate"), [&](const std::string& m) {
    stages.push_back(json::parse(m)["stage"]);
    return true;
  });
  EXPECT_EQ(stages, (std::vector<std::string>{"query_expansion",
                                              "keyword_search", "done"}));
  // A sink that reports a dead connection stops the pipeline.
  int sent = 0;
  service.performStreamingSearch(request("climate"), [&](const std::string&) {
    ++sent;
    return false;
  });
  EXPECT_EQ(sent, 1);
}

TEST(PipelineWebSocket, BackToBackQueriesAreFifoAndNotInterleaved) {
  TempDir dir;
  writeDataset(dir);
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["sea"]})");
  fake.setDelayMs(150);  // keeps query 1 running while 2 and 3 arrive
  SearchPipelineService service(configFor(dir, fake.url(), 5000),
                                {nullptr, nullptr, 4});
  ASSERT_FALSE(service.buildIndex(1).error.has_value());
  Collector collector;
  auto session = collector.makeSession();
  service.handleWebSocketMessage(session, R"({"query":"climate"})", false);
  service.handleWebSocketMessage(session, R"({"query":"pasta"})", false);
  service.handleWebSocketMessage(session, R"({"query":"global"})", false);
  ASSERT_TRUE(collector.waitFor(9));
  const auto messages = collector.messages();
  const char* originals[] = {"climate", "pasta", "global"};
  for (int q = 0; q < 3; ++q) {
    EXPECT_EQ(messages[q * 3]["stage"], "query_expansion");
    EXPECT_EQ(messages[q * 3]["data"]["original"], originals[q]);
    EXPECT_EQ(messages[q * 3 + 1]["stage"], "keyword_search");
    EXPECT_EQ(messages[q * 3 + 2]["stage"], "done");
  }
}

TEST(PipelineWebSocket, DifferentConnectionsRunInParallel) {
  TempDir dir;
  writeDataset(dir);
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["sea"]})");
  fake.setDelayMs(400);
  SearchPipelineService service(configFor(dir, fake.url(), 5000),
                                {nullptr, nullptr, 4});
  ASSERT_FALSE(service.buildIndex(1).error.has_value());
  const auto started = std::chrono::steady_clock::now();
  Collector collectors[3];
  std::shared_ptr<WebSocketSession> sessions[3];
  for (int i = 0; i < 3; ++i) {
    sessions[i] = collectors[i].makeSession();
    service.handleWebSocketMessage(sessions[i], R"({"query":"climate"})",
                                   false);
  }
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(collectors[i].waitFor(3));
  // 3 connections x 400 ms would take >= 1200 ms if serialised.
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::milliseconds(1000));
}

TEST(PipelineWebSocket, NothingIsSentAfterClose) {
  TempDir dir;
  writeDataset(dir);
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["sea"]})");
  fake.setDelayMs(300);
  SearchPipelineService service(configFor(dir, fake.url(), 5000),
                                {nullptr, nullptr, 2});
  ASSERT_FALSE(service.buildIndex(1).error.has_value());
  std::atomic<int> sends{0};
  std::atomic<bool> closedFlag{false};
  std::atomic<int> sendsAfterClose{0};
  auto session = std::make_shared<WebSocketSession>([&](std::string) {
    sends.fetch_add(1);
    if (closedFlag.load()) sendsAfterClose.fetch_add(1);
  });
  service.handleWebSocketMessage(session, R"({"query":"climate"})", false);
  service.handleWebSocketMessage(session, R"({"query":"pasta"})", false);
  std::this_thread::sleep_for(
      std::chrono::milliseconds(50));  // query 1 running
  session->close();
  closedFlag = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  EXPECT_EQ(sends.load(), 0);
  EXPECT_EQ(sendsAfterClose.load(), 0);
  EXPECT_FALSE(session->send("x"));
  EXPECT_TRUE(session->isClosed());
  // Messages after close are dropped silently.
  service.handleWebSocketMessage(session, R"({"query":"climate"})", false);
}

TEST(PipelineWebSocket, ServiceDestructionWithBusyAndQueuedSessions) {
  TempDir dir;
  writeDataset(dir);
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["sea"]})");
  fake.setDelayMs(200);
  auto session = std::make_shared<WebSocketSession>([](std::string) {});
  {
    SearchPipelineService service(configFor(dir, fake.url(), 5000),
                                  {nullptr, nullptr, 2});
    ASSERT_FALSE(service.buildIndex(1).error.has_value());
    for (int i = 0; i < 5; ++i) {
      service.handleWebSocketMessage(session, R"({"query":"climate"})", false);
    }
  }  // must neither hang nor crash
  SUCCEED();
}

// --- expansion limiter (docs/API.md §5.4) and WebSocket fast path ------------

namespace {
template <typename Predicate>
bool waitUntil(Predicate predicate, int ms = 5000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}
}  // namespace

TEST(ExpansionLimiter, AtMostEightInFlightAcrossRestAndWebSocket) {
  TempDir dir;
  writeDataset(dir);
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["sea"]})");
  fake.blockGenerate();
  SearchPipelineService service(configFor(dir, fake.url(), 20000),
                                {nullptr, nullptr, 8});
  ASSERT_FALSE(service.buildIndex(1).error.has_value());

  std::atomic<int> fallbacks{0};
  std::atomic<int> llm{0};
  std::vector<std::thread> restThreads;
  for (int i = 0; i < 6; ++i) {
    restThreads.emplace_back([&] {
      auto outcome = service.performKeywordSearch(request("climate"));
      const auto& response = std::get<SearchResponse>(outcome);
      (response.expansion.source == search::ExpansionSource::kFallback
           ? fallbacks
           : llm)
          .fetch_add(1);
    });
  }
  Collector collectors[6];
  std::shared_ptr<WebSocketSession> sessions[6];
  for (int i = 0; i < 6; ++i) {
    sessions[i] = collectors[i].makeSession();
    service.handleWebSocketMessage(sessions[i], R"({"query":"climate"})",
                                   false);
  }
  // Exactly 8 reach the fake; the other 4 shed load without waiting.
  ASSERT_TRUE(waitUntil([&] { return fake.generateCalls() == 8; }));
  int wsFallbacks = 0;
  ASSERT_TRUE(waitUntil([&] {
    wsFallbacks = 0;
    for (auto& c : collectors) {
      const auto messages = c.messages();
      if (!messages.empty() && messages[0]["data"]["expansion"] == "fallback")
        ++wsFallbacks;
    }
    return fallbacks.load() + wsFallbacks == 4;
  }));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(fake.generateCalls(), 8);

  fake.release();
  for (auto& t : restThreads) t.join();
  for (auto& c : collectors) ASSERT_TRUE(c.waitFor(3));
  EXPECT_EQ(llm.load() + fallbacks.load(), 6);

  // Slots were returned: a new expansion reaches the fake again.
  auto outcome = service.performKeywordSearch(request("climate"));
  EXPECT_EQ(std::get<SearchResponse>(outcome).expansion.source,
            search::ExpansionSource::kLlm);
  EXPECT_EQ(fake.generateCalls(), 9);
}

TEST(ExpansionLimiter, TimeoutsReleaseSlots) {
  TempDir dir;
  writeDataset(dir);
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["sea"]})");
  fake.setDelayMs(500);  // longer than the 100 ms client timeout
  SearchPipelineService service(configFor(dir, fake.url(), 100));
  ASSERT_FALSE(service.buildIndex(1).error.has_value());
  // 12 sequential timeouts would exhaust 8 slots if they leaked.
  for (int i = 0; i < 12; ++i) {
    auto outcome = service.performKeywordSearch(request("climate"));
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(outcome));
    EXPECT_EQ(std::get<SearchResponse>(outcome).expansion.source,
              search::ExpansionSource::kFallback);
  }
  EXPECT_EQ(fake.generateCalls(), 12);  // every call reached the fake
}

TEST(PipelineWebSocket, InvalidFrameOnIdleSessionBypassesBusyPool) {
  TempDir dir;
  writeDataset(dir);
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["sea"]})");
  fake.blockGenerate();
  SearchPipelineService service(configFor(dir, fake.url(), 20000),
                                {nullptr, nullptr, 1});  // one pool worker
  ASSERT_FALSE(service.buildIndex(1).error.has_value());
  Collector busy, idle;
  auto busySession = busy.makeSession();
  auto idleSession = idle.makeSession();
  service.handleWebSocketMessage(busySession, R"({"query":"climate"})", false);
  ASSERT_TRUE(waitUntil([&] { return fake.generateCalls() == 1; }));
  // The only worker is blocked; the error must still arrive, synchronously.
  service.handleWebSocketMessage(idleSession, "not json", false);
  const auto messages = idle.messages();
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0]["stage"], "error");
  EXPECT_EQ(messages[0]["data"]["code"], "invalid_json");
  service.handleWebSocketMessage(idleSession, "\x01\x02", true);
  EXPECT_EQ(idle.messages().size(), 2u);
  fake.release();
  ASSERT_TRUE(busy.waitFor(3));
}

TEST(PipelineWebSocket, InvalidFrameBehindPendingQueryKeepsPosition) {
  TempDir dir;
  writeDataset(dir);
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["sea"]})");
  fake.blockGenerate();
  SearchPipelineService service(configFor(dir, fake.url(), 20000),
                                {nullptr, nullptr, 2});
  ASSERT_FALSE(service.buildIndex(1).error.has_value());
  Collector collector;
  auto session = collector.makeSession();
  service.handleWebSocketMessage(session, R"({"query":"climate"})", false);
  ASSERT_TRUE(waitUntil([&] { return fake.generateCalls() == 1; }));
  service.handleWebSocketMessage(session, "not json", false);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_TRUE(collector.messages().empty());  // error waits behind the query
  fake.release();
  ASSERT_TRUE(collector.waitFor(4));
  EXPECT_EQ(collector.stages(),
            (std::vector<std::string>{"query_expansion", "keyword_search",
                                      "done", "error"}));
}
