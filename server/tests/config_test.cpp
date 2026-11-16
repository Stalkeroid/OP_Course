#include "search/config.h"

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace {

search::EnvLookup lookupFrom(const std::map<std::string, std::string>& env) {
  return [&env](const char* name) -> const char* {
    const auto it = env.find(name);
    return it == env.end() ? nullptr : it->second.c_str();
  };
}

}  // namespace

TEST(ConfigTest, Defaults) {
  std::map<std::string, std::string> env;
  const search::Config config = search::loadConfig(lookupFrom(env));
  EXPECT_EQ(config.port, 8080);
  EXPECT_EQ(config.datasetPath, "/data/dataset");
  EXPECT_EQ(config.ollamaUrl, "http://ollama:11434");
  EXPECT_EQ(config.ollamaModel, "llama3.2:1b");
  EXPECT_EQ(config.ollamaTimeoutMs, 15000);
  EXPECT_EQ(config.webRoot, "/app/web");
}

TEST(ConfigTest, Overrides) {
  std::map<std::string, std::string> env = {{"PORT", "9090"},
                                            {"DATASET_PATH", "/tmp/ds"},
                                            {"OLLAMA_URL", "http://x:1"},
                                            {"OLLAMA_MODEL", "m:1"},
                                            {"OLLAMA_TIMEOUT_MS", "250"},
                                            {"WEB_ROOT", "/tmp/web"}};
  const search::Config config = search::loadConfig(lookupFrom(env));
  EXPECT_EQ(config.port, 9090);
  EXPECT_EQ(config.datasetPath, "/tmp/ds");
  EXPECT_EQ(config.ollamaUrl, "http://x:1");
  EXPECT_EQ(config.ollamaModel, "m:1");
  EXPECT_EQ(config.ollamaTimeoutMs, 250);
  EXPECT_EQ(config.webRoot, "/tmp/web");
}

TEST(ConfigTest, EmptyValueKeepsDefault) {
  std::map<std::string, std::string> env = {{"PORT", ""}, {"OLLAMA_URL", ""}};
  const search::Config config = search::loadConfig(lookupFrom(env));
  EXPECT_EQ(config.port, 8080);
  EXPECT_EQ(config.ollamaUrl, "http://ollama:11434");
}

TEST(ConfigTest, MalformedNumbersThrow) {
  for (const char* bad : {"abc", "0", "70000", "80x", "-1"}) {
    std::map<std::string, std::string> env = {{"PORT", bad}};
    EXPECT_THROW(search::loadConfig(lookupFrom(env)), std::invalid_argument)
        << bad;
  }
  std::map<std::string, std::string> env = {{"OLLAMA_TIMEOUT_MS", "0"}};
  EXPECT_THROW(search::loadConfig(lookupFrom(env)), std::invalid_argument);
}

TEST(ConfigTest, WorkerThreadsAtLeastSixteen) {
  EXPECT_GE(search::defaultWorkerThreads(), 16u);
}
