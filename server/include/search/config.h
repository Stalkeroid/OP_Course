// Server configuration — docs/API.md §12. Read only from environment
// variables; every variable has a default.
#pragma once

#include <functional>
#include <string>

namespace search {

struct Config {
  int port = 8080;                                // PORT
  std::string datasetPath = "/data/dataset";      // DATASET_PATH
  std::string ollamaUrl = "http://ollama:11434";  // OLLAMA_URL
  std::string ollamaModel = "llama3.2:1b";        // OLLAMA_MODEL
  int ollamaTimeoutMs = 15000;                    // OLLAMA_TIMEOUT_MS
  std::string webRoot = "/app/web";               // WEB_ROOT
};

// Returns the value of an environment variable or nullptr when unset.
using EnvLookup = std::function<const char*(const char* name)>;

// Reads the configuration through `lookup` (defaults to std::getenv). An unset
// or empty variable keeps its default. A malformed number (PORT outside
// [1, 65535], OLLAMA_TIMEOUT_MS <= 0, non-numeric) throws
// std::invalid_argument naming the variable.
Config loadConfig(const EnvLookup& lookup);
Config loadConfigFromEnvironment();

// One-line human-readable description for the startup log.
std::string describeConfig(const Config& config);

// Number of Crow worker threads: max(16, 2 x hardware_concurrency). HTTP
// handlers that block (index build, query expansion) must not starve
// /api/health and /api/stats.
unsigned defaultWorkerThreads();

}  // namespace search
