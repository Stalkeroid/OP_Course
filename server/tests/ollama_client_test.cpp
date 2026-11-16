#include "search/ollama_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "fake_ollama.h"

using search::OllamaClient;
using search_test::FakeOllama;
using Terms = std::vector<std::string>;

namespace {

std::optional<Terms> parseAnswer(const std::string& modelAnswer) {
  return OllamaClient::parseGenerateResponse(FakeOllama::wrap(modelAnswer));
}

int unusedPort() {
  httplib::Server server;
  const int port = server.bind_to_any_port("127.0.0.1");
  return port;  // closed again when `server` goes out of scope
}

}  // namespace

// --- parsing (no sockets) ---------------------------------------------------

TEST(OllamaParse, ValidTerms) {
  const auto terms = parseAnswer(R"({"terms":["global warming","weather"]})");
  ASSERT_TRUE(terms.has_value());
  EXPECT_EQ(*terms, (Terms{"global warming", "weather"}));
}

TEST(OllamaParse, EmptyTermsIsStillAnAnswer) {
  const auto terms = parseAnswer(R"({"terms":[]})");
  ASSERT_TRUE(terms.has_value());
  EXPECT_TRUE(terms->empty());
}

TEST(OllamaParse, ExtraTextAroundObjectIsTolerated) {
  const auto terms = parseAnswer(
      "Sure! Here you go: {\"terms\": [\"a1\", \"b2\"]} Hope it helps.");
  ASSERT_TRUE(terms.has_value());
  EXPECT_EQ(*terms, (Terms{"a1", "b2"}));
}

TEST(OllamaParse, CodeFenceIsStripped) {
  const auto terms = parseAnswer("```json\n{\"terms\": [\"x1\", \"y2\"]}\n```");
  ASSERT_TRUE(terms.has_value());
  EXPECT_EQ(*terms, (Terms{"x1", "y2"}));
}

TEST(OllamaParse, NonStringElementsIgnored) {
  const auto terms =
      parseAnswer(R"({"terms":["ok",5,null,["n"],{"a":1},"fine"]})");
  ASSERT_TRUE(terms.has_value());
  EXPECT_EQ(*terms, (Terms{"ok", "fine"}));
}

TEST(OllamaParse, WrongShapesAreFailures) {
  EXPECT_FALSE(parseAnswer(R"({"terms":"not an array"})").has_value());
  EXPECT_FALSE(parseAnswer(R"({"keywords":["a"]})").has_value());
  EXPECT_FALSE(parseAnswer(R"(["a","b"])").has_value());
  EXPECT_FALSE(parseAnswer("no json at all").has_value());
  EXPECT_FALSE(parseAnswer("").has_value());
  EXPECT_FALSE(parseAnswer("{\"terms\": [").has_value());
}

TEST(OllamaParse, OuterBodyProblems) {
  EXPECT_FALSE(OllamaClient::parseGenerateResponse("").has_value());
  EXPECT_FALSE(OllamaClient::parseGenerateResponse("not json").has_value());
  EXPECT_FALSE(OllamaClient::parseGenerateResponse("[]").has_value());
  EXPECT_FALSE(
      OllamaClient::parseGenerateResponse(R"({"done":true})").has_value());
  EXPECT_FALSE(
      OllamaClient::parseGenerateResponse(R"({"response":5})").has_value());
}

TEST(OllamaParse, TagsContainModel) {
  const std::string tags =
      R"({"models":[{"name":"llama3.2:1b"},{"name":"mistral:latest"}]})";
  EXPECT_TRUE(OllamaClient::tagsContainModel(tags, "llama3.2:1b"));
  EXPECT_TRUE(OllamaClient::tagsContainModel(tags, "mistral"));
  EXPECT_TRUE(OllamaClient::tagsContainModel(tags, "mistral:latest"));
  EXPECT_FALSE(OllamaClient::tagsContainModel(tags, "llama3.2"));
  EXPECT_FALSE(OllamaClient::tagsContainModel(tags, "llama3.2:3b"));
  EXPECT_FALSE(OllamaClient::tagsContainModel(R"({"models":[]})", "mistral"));
  EXPECT_FALSE(OllamaClient::tagsContainModel("garbage", "mistral"));
  EXPECT_FALSE(
      OllamaClient::tagsContainModel(R"({"models":[{"name":5}]})", "x"));
}

TEST(OllamaRequest, BodyMatchesContract) {
  const auto body =
      nlohmann::json::parse(OllamaClient::buildGenerateRequestBody(
          "llama3.2:1b", "climate \"change\""));
  EXPECT_EQ(body["model"], "llama3.2:1b");
  EXPECT_EQ(body["stream"], false);
  EXPECT_EQ(body["format"], "json");
  EXPECT_EQ(body["options"]["temperature"], 0);
  const std::string prompt = body["prompt"];
  EXPECT_NE(prompt.find("climate \"change\""), std::string::npos);
  EXPECT_EQ(prompt.find("{query}"), std::string::npos);
  EXPECT_NE(prompt.find("terms"), std::string::npos);
}

// --- against a fake Ollama server -------------------------------------------

TEST(OllamaClientFake, SuccessfulExpansion) {
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["global","warming"]})");
  OllamaClient client(fake.url(), "llama3.2:1b", 2000);
  const auto terms = client.expandQuery("climate change");
  ASSERT_TRUE(terms.has_value());
  EXPECT_EQ(*terms, (Terms{"global", "warming"}));
  const auto sent = nlohmann::json::parse(fake.lastRequest());
  EXPECT_EQ(sent["model"], "llama3.2:1b");
  EXPECT_EQ(sent["stream"], false);
  EXPECT_NE(std::string(sent["prompt"]).find("climate change"),
            std::string::npos);
}

TEST(OllamaClientFake, TrailingSlashInUrlIsAccepted) {
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["a1"]})");
  OllamaClient client(fake.url() + "/", "llama3.2:1b", 2000);
  EXPECT_TRUE(client.expandQuery("anything").has_value());
}

TEST(OllamaClientFake, SlowAnswerTimesOut) {
  FakeOllama fake;
  fake.setModelAnswer(R"({"terms":["late"]})");
  fake.setDelayMs(1500);
  OllamaClient client(fake.url(), "llama3.2:1b", 300);
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(client.expandQuery("climate").has_value());
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, std::chrono::milliseconds(1200));
}

TEST(OllamaClientFake, ConnectionRefusedFallsBack) {
  OllamaClient client("http://127.0.0.1:" + std::to_string(unusedPort()),
                      "llama3.2:1b", 1000);
  EXPECT_FALSE(client.expandQuery("climate").has_value());
}

TEST(OllamaClientFake, UnresolvableHostFallsBack) {
  OllamaClient client("http://no-such-host.invalid:11434", "llama3.2:1b", 1000);
  EXPECT_FALSE(client.expandQuery("climate").has_value());
}

TEST(OllamaClientFake, NonOkStatusAndGarbageFallBack) {
  FakeOllama fake;
  OllamaClient client(fake.url(), "llama3.2:1b", 1000);
  fake.setGenerateRaw(404, R"({"error":"model not found"})");
  EXPECT_FALSE(client.expandQuery("climate").has_value());
  fake.setGenerateRaw(200, "<html>oops</html>");
  EXPECT_FALSE(client.expandQuery("climate").has_value());
  fake.setGenerateRaw(200, FakeOllama::wrap("I cannot do that."));
  EXPECT_FALSE(client.expandQuery("climate").has_value());
}

TEST(OllamaClientFake, HealthTrueWhenModelListed) {
  FakeOllama fake;
  OllamaClient client(fake.url(), "llama3.2:1b", 1000);
  EXPECT_TRUE(client.checkHealth());
}

TEST(OllamaClientFake, HealthFalseWhenModelMissing) {
  FakeOllama fake;
  fake.setTags(R"({"models":[{"name":"other:1b"}]})");
  OllamaClient client(fake.url(), "llama3.2:1b", 1000);
  EXPECT_FALSE(client.checkHealth());
}

TEST(OllamaClientFake, HealthFalseWhenDown) {
  OllamaClient client("http://127.0.0.1:" + std::to_string(unusedPort()),
                      "llama3.2:1b", 1000);
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(client.checkHealth());
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(3));
}

TEST(OllamaClientFake, HealthResultIsCachedBriefly) {
  FakeOllama fake;
  OllamaClient client(fake.url(), "llama3.2:1b", 1000);
  EXPECT_TRUE(client.checkHealth());
  fake.setTags(R"({"models":[]})");
  EXPECT_TRUE(client.checkHealth());  // still the cached answer (< 5 s)
}

TEST(OllamaClientFake, HealthIsThreadSafe) {
  FakeOllama fake;
  OllamaClient client(fake.url(), "llama3.2:1b", 1000);
  EXPECT_TRUE(client.checkHealth());  // warm the cache: first check is single
  std::vector<std::thread> threads;
  std::atomic<int> trues{0};
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < 20; ++j) {
        if (client.checkHealth()) trues.fetch_add(1);
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(trues.load(), 160);
}

TEST(OllamaClientFake, HealthIsSingleFlight) {
  FakeOllama fake;
  fake.blockTags();
  OllamaClient client(fake.url(), "llama3.2:1b", 5000);
  std::atomic<bool> proberResult{false};
  std::atomic<bool> proberDone{false};
  std::thread prober([&] {
    proberResult = client.checkHealth();
    proberDone = true;
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (fake.tagsCalls() < 1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_EQ(fake.tagsCalls(), 1);

  std::vector<std::thread> threads;
  std::atomic<int> falses{0};
  std::atomic<long> slowestUs{0};
  for (int i = 0; i < 20; ++i) {
    threads.emplace_back([&] {
      const auto t0 = std::chrono::steady_clock::now();
      if (!client.checkHealth()) falses.fetch_add(1);
      const long us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
      long prev = slowestUs.load();
      while (us > prev && !slowestUs.compare_exchange_weak(prev, us)) {
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(falses.load(), 20);  // no completed check yet -> false
  EXPECT_LT(slowestUs.load(), 500000);
  EXPECT_FALSE(proberDone.load());
  EXPECT_EQ(fake.tagsCalls(), 1);

  fake.release();
  prober.join();
  EXPECT_TRUE(proberResult.load());
  EXPECT_TRUE(client.checkHealth());  // cache updated
  EXPECT_EQ(fake.tagsCalls(), 1);
}
