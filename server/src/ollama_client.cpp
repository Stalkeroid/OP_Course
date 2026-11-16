#include "search/ollama_client.h"

#include <httplib.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <utility>

namespace search {

using nlohmann::json;

namespace {

std::string trimTrailingSlashes(std::string url) {
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

void applyTimeouts(httplib::Client& client, int timeoutMs) {
  const std::chrono::milliseconds timeout(timeoutMs);
  client.set_connection_timeout(timeout);
  client.set_read_timeout(timeout);
  client.set_write_timeout(timeout);
  // Overall deadline of the request: connect + read combined.
  client.set_max_timeout(timeout);
}

// Strips a Markdown code fence (``` or ```json) if the text starts with one.
std::string stripCodeFence(const std::string& text) {
  const auto first = text.find("```");
  if (first == std::string::npos) return text;
  auto bodyStart = text.find('\n', first);
  if (bodyStart == std::string::npos) return text;
  const auto last = text.find("```", bodyStart);
  if (last == std::string::npos) return text.substr(bodyStart + 1);
  return text.substr(bodyStart + 1, last - bodyStart - 1);
}

// Finds a JSON object in model output: the whole text, or the span from the
// first '{' to the last '}'.
json parseModelJson(const std::string& raw) {
  json whole = json::parse(raw, nullptr, false);
  if (whole.is_object()) return whole;
  const std::string unfenced = stripCodeFence(raw);
  json fenced = json::parse(unfenced, nullptr, false);
  if (fenced.is_object()) return fenced;
  const auto open = unfenced.find('{');
  const auto close = unfenced.rfind('}');
  if (open != std::string::npos && close != std::string::npos && close > open) {
    json span =
        json::parse(unfenced.substr(open, close - open + 1), nullptr, false);
    if (span.is_object()) return span;
  }
  return json(json::value_t::discarded);
}

}  // namespace

constexpr std::chrono::seconds OllamaClient::kHealthCacheTtl;

OllamaClient::OllamaClient(std::string baseUrl, std::string model,
                           int timeoutMs)
    : baseUrl_(trimTrailingSlashes(std::move(baseUrl))),
      model_(std::move(model)),
      timeoutMs_(timeoutMs) {}

std::string OllamaClient::buildPrompt(const std::string& query) {
  std::string prompt = kExpansionPromptTemplate;
  const std::string placeholder = "{query}";
  const auto pos = prompt.rfind(placeholder);
  prompt.replace(pos, placeholder.size(), query);
  return prompt;
}

std::string OllamaClient::buildGenerateRequestBody(const std::string& model,
                                                   const std::string& query) {
  json body = {{"model", model},
               {"prompt", buildPrompt(query)},
               {"stream", false},
               {"format", "json"},
               {"options", {{"temperature", 0}}}};
  // Queries are valid UTF-8 (validated), but never throw on odd input.
  return body.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::optional<std::vector<std::string>> OllamaClient::parseGenerateResponse(
    const std::string& responseBody) {
  json outer = json::parse(responseBody, nullptr, false);
  if (!outer.is_object()) return std::nullopt;
  const auto it = outer.find("response");
  if (it == outer.end() || !it->is_string()) return std::nullopt;

  json inner = parseModelJson(it->get_ref<const std::string&>());
  if (!inner.is_object()) return std::nullopt;
  const auto terms = inner.find("terms");
  if (terms == inner.end() || !terms->is_array()) return std::nullopt;

  std::vector<std::string> result;
  for (const auto& term : *terms) {
    if (term.is_string()) result.push_back(term.get<std::string>());
  }
  return result;
}

bool OllamaClient::tagsContainModel(const std::string& tagsBody,
                                    const std::string& model) {
  json root = json::parse(tagsBody, nullptr, false);
  if (!root.is_object()) return false;
  const auto models = root.find("models");
  if (models == root.end() || !models->is_array()) return false;
  const bool hasTag = model.find(':') != std::string::npos;
  const std::string withLatest = model + ":latest";
  for (const auto& entry : *models) {
    if (!entry.is_object()) continue;
    const auto name = entry.find("name");
    if (name == entry.end() || !name->is_string()) continue;
    const auto& value = name->get_ref<const std::string&>();
    if (value == model || (!hasTag && value == withLatest)) return true;
  }
  return false;
}

std::optional<std::vector<std::string>> OllamaClient::expandQuery(
    const std::string& originalQuery) const {
  try {
    httplib::Client client(baseUrl_);
    applyTimeouts(client, timeoutMs_);
    const auto response = client.Post(
        "/api/generate", buildGenerateRequestBody(model_, originalQuery),
        "application/json");
    if (!response || response->status != 200) return std::nullopt;
    return parseGenerateResponse(response->body);
  } catch (...) {
    return std::nullopt;
  }
}

bool OllamaClient::probeHealth() const {
  try {
    httplib::Client client(baseUrl_);
    applyTimeouts(client, kHealthTimeoutMs);
    const auto response = client.Get("/api/tags");
    if (!response || response->status != 200) return false;
    return tagsContainModel(response->body, model_);
  } catch (...) {
    return false;
  }
}

bool OllamaClient::checkHealth() {
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(healthMutex_);
    if (hasHealth_ && now - healthAt_ < kHealthCacheTtl) return healthValue_;
  }
  // Probe without holding the lock: a hanging Ollama must not serialise the
  // health endpoint behind one slow request.
  const bool healthy = probeHealth();
  std::lock_guard<std::mutex> lock(healthMutex_);
  hasHealth_ = true;
  healthValue_ = healthy;
  healthAt_ = std::chrono::steady_clock::now();
  return healthy;
}

}  // namespace search
