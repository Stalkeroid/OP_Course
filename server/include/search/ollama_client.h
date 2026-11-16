// OllamaClient — query expansion and health check against Ollama
// (docs/API.md §5, §6). Uses cpp-httplib. Every failure is reported as
// "no expansion" (std::nullopt) / "not healthy"; nothing here throws to the
// caller and nothing becomes an HTTP error for the search client.
#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace search {

// The single place where the expansion prompt text lives. "{query}" is
// replaced by the user's query (see OllamaClient::buildPrompt).
inline constexpr char kExpansionPromptTemplate[] =
    "You are a search query expansion engine. Given a search query, list "
    "keywords and synonyms that are closely related to it.\n"
    "Rules:\n"
    "- English only.\n"
    "- Each term is a single word or a very short phrase.\n"
    "- At most 10 terms, most relevant first.\n"
    "- Do not repeat the words of the query itself.\n"
    "- No explanations, no extra text.\n"
    "Answer only with a JSON object of this exact form: "
    "{\"terms\": [\"term1\", \"term2\"]}\n"
    "Query: {query}";

class OllamaClient {
 public:
  // baseUrl e.g. "http://ollama:11434"; timeoutMs bounds one expansion call
  // (connect + read combined, docs/API.md §5.1).
  OllamaClient(std::string baseUrl, std::string model, int timeoutMs);

  // POST {baseUrl}/api/generate. Returns the raw terms of the answer's
  // {"terms": [...]} array (before §4.2 tokenisation) or std::nullopt when
  // the call failed or the answer is unusable (caller reports "fallback").
  // An empty vector is a valid answer ("llm" with no extra terms).
  std::optional<std::vector<std::string>> expandQuery(
      const std::string& originalQuery) const;

  // True iff GET {baseUrl}/api/tags answers 200 within 2000 ms and lists the
  // model (§6.2). The result is cached for at most kHealthCacheTtl; safe to
  // call from many threads.
  bool checkHealth();

  // --- pure helpers (unit-tested without sockets) ---------------------------
  static std::string buildPrompt(const std::string& query);
  // The JSON body sent to /api/generate (§5.1).
  static std::string buildGenerateRequestBody(const std::string& model,
                                              const std::string& query);
  // Parses the body of a /api/generate answer: outer object with a string
  // field "response" that contains {"terms":[...]} (code fences and text
  // around the object are tolerated). Non-string terms are ignored.
  static std::optional<std::vector<std::string>> parseGenerateResponse(
      const std::string& responseBody);
  // True iff the /api/tags body lists `model` in models[].name (exact, or
  // model + ":latest" when `model` has no tag).
  static bool tagsContainModel(const std::string& tagsBody,
                               const std::string& model);

  static constexpr std::chrono::seconds kHealthCacheTtl{5};
  static constexpr int kHealthTimeoutMs = 2000;

 private:
  bool probeHealth() const;

  std::string baseUrl_;
  std::string model_;
  int timeoutMs_;

  std::mutex healthMutex_;
  bool hasHealth_ = false;
  bool healthValue_ = false;
  std::chrono::steady_clock::time_point healthAt_;
};

}  // namespace search
