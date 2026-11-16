#include "search/config.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>

namespace search {

namespace {

std::string readString(const EnvLookup& lookup, const char* name,
                       const std::string& fallback) {
  const char* value = lookup(name);
  if (value == nullptr || *value == '\0') return fallback;
  return value;
}

int readInt(const EnvLookup& lookup, const char* name, int fallback, int min,
            int max) {
  const char* value = lookup(name);
  if (value == nullptr || *value == '\0') return fallback;
  std::size_t used = 0;
  long long parsed = 0;
  try {
    parsed = std::stoll(value, &used);
  } catch (const std::exception&) {
    used = 0;
  }
  if (used == 0 || value[used] != '\0' || parsed < min || parsed > max) {
    throw std::invalid_argument(std::string("invalid value for ") + name +
                                ": '" + value + "' (expected integer in [" +
                                std::to_string(min) + ", " +
                                std::to_string(max) + "])");
  }
  return static_cast<int>(parsed);
}

}  // namespace

Config loadConfig(const EnvLookup& lookup) {
  Config config;
  config.port = readInt(lookup, "PORT", config.port, 1, 65535);
  config.datasetPath = readString(lookup, "DATASET_PATH", config.datasetPath);
  config.ollamaUrl = readString(lookup, "OLLAMA_URL", config.ollamaUrl);
  config.ollamaModel = readString(lookup, "OLLAMA_MODEL", config.ollamaModel);
  config.ollamaTimeoutMs = readInt(lookup, "OLLAMA_TIMEOUT_MS",
                                   config.ollamaTimeoutMs, 1, 3600 * 1000);
  config.webRoot = readString(lookup, "WEB_ROOT", config.webRoot);
  return config;
}

Config loadConfigFromEnvironment() {
  return loadConfig([](const char* name) { return std::getenv(name); });
}

std::string describeConfig(const Config& config) {
  return "port=" + std::to_string(config.port) +
         " dataset_path=" + config.datasetPath +
         " ollama_url=" + config.ollamaUrl +
         " ollama_model=" + config.ollamaModel +
         " ollama_timeout_ms=" + std::to_string(config.ollamaTimeoutMs) +
         " web_root=" + config.webRoot;
}

unsigned defaultWorkerThreads() {
  return std::max(16u, 2u * std::thread::hardware_concurrency());
}

}  // namespace search
