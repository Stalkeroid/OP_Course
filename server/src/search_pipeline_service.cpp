#include "search/search_pipeline_service.h"

#include <chrono>
#include <exception>
#include <nlohmann/json.hpp>
#include <thread>
#include <utility>

#include "search/snippet.h"
#include "search/tokenizer.h"

namespace search {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

std::int64_t elapsedMs(Clock::time_point since) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                               since)
      .count();
}

std::string dumpJson(const json& value) {
  return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

ApiError internalError() {
  return ApiError{500, "internal_error", "unexpected internal error"};
}

ApiError indexNotBuilt() {
  return ApiError{503, "index_not_built",
                  "index is not built; call POST /api/index first"};
}

ApiError binaryFrameError() {
  return ApiError{400, "invalid_json", "binary frames are not supported"};
}

// Error of a frame that would be rejected by validation, if any.
std::optional<ApiError> validationError(const std::string& payload,
                                        bool isBinary) {
  if (isBinary) return binaryFrameError();
  auto parsed = parseSearchRequest(payload, MessageSource::kWebSocketFrame);
  if (auto* error = std::get_if<ApiError>(&parsed)) return std::move(*error);
  return std::nullopt;
}

IndexBuildFunction defaultBuilder() {
  return [](const std::filesystem::path& path, int threads) {
    return IndexBuilder::buildIndex(path, threads);
  };
}

// RAII holder of one expansion slot (§5.4). Never waits.
class ExpansionSlot {
 public:
  explicit ExpansionSlot(std::atomic<int>& counter) : counter_(counter) {
    int current = counter_.load();
    while (current < SearchPipelineService::kMaxConcurrentExpansions) {
      if (counter_.compare_exchange_weak(current, current + 1)) {
        held_ = true;
        return;
      }
    }
  }
  ~ExpansionSlot() {
    if (held_) counter_.fetch_sub(1);
  }
  ExpansionSlot(const ExpansionSlot&) = delete;
  ExpansionSlot& operator=(const ExpansionSlot&) = delete;
  bool held() const { return held_; }

 private:
  std::atomic<int>& counter_;
  bool held_ = false;
};

}  // namespace

const char* expansionSourceName(ExpansionSource source) {
  switch (source) {
    case ExpansionSource::kLlm:
      return "llm";
    case ExpansionSource::kFallback:
      return "fallback";
    case ExpansionSource::kDisabled:
      break;
  }
  return "disabled";
}

json resultsToJson(const std::vector<SearchResult>& results) {
  json array = json::array();
  for (const auto& result : results) {
    array.push_back({{"doc_id", result.docId},
                     {"score", result.score},
                     {"snippet", result.snippet}});
  }
  return array;
}

// --- WebSocketSession --------------------------------------------------------

WebSocketSession::WebSocketSession(std::function<void(std::string)> sendText)
    : sendText_(std::move(sendText)) {}

bool WebSocketSession::send(const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) return false;
  sendText_(message);
  return true;
}

void WebSocketSession::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  closed_ = true;
  queue_.clear();
}

bool WebSocketSession::isClosed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return closed_;
}

// --- SearchPipelineService ---------------------------------------------------

SearchPipelineService::SearchPipelineService(Config config,
                                             PipelineOptions options)
    : config_(std::move(config)),
      ollama_(options.ollama ? std::move(options.ollama)
                             : std::make_shared<OllamaClient>(
                                   config_.ollamaUrl, config_.ollamaModel,
                                   config_.ollamaTimeoutMs)),
      builder_(options.builder ? std::move(options.builder) : defaultBuilder()),
      webSocketPool_(options.webSocketWorkers > 0
                         ? options.webSocketWorkers
                         : static_cast<int>(defaultWorkerThreads())) {}

SearchPipelineService::~SearchPipelineService() {
  stopping_.store(true);
  webSocketPool_.shutdown();  // queued drain tasks return immediately
}

std::shared_ptr<const InvertedIndex> SearchPipelineService::snapshot() const {
  std::lock_guard<std::mutex> lock(indexMutex_);
  return servedIndex_;
}

BuildResult SearchPipelineService::buildIndex(int threads) {
  BuildResult result;
  bool expected = false;
  if (!building_.compare_exchange_strong(expected, true)) {
    result.error =
        ApiError{409, "index_in_progress", "an index build is already running"};
    return result;
  }
  struct ClearFlag {
    std::atomic<bool>& flag;
    ~ClearFlag() { flag.store(false); }
  } clearFlag{building_};

  try {
    BuildOutcome outcome = builder_(config_.datasetPath, threads);
    if (!outcome.ok()) {
      result.error = ApiError{
          500, buildErrorCode(outcome.error),
          outcome.message.empty() ? "index build failed" : outcome.message};
      return result;
    }
    result.stats = outcome.stats;
    std::shared_ptr<const InvertedIndex> built = std::move(outcome.index);
    std::shared_ptr<const InvertedIndex> old;
    {
      std::lock_guard<std::mutex> lock(indexMutex_);
      old = std::move(servedIndex_);  // atomic swap; the old index stays alive
      servedIndex_ = std::move(built);  // for searches still using it
    }
    old.reset();  // destroyed outside the lock (can take tens of ms)
  } catch (const std::exception&) {
    result.error = internalError();
  } catch (...) {
    result.error = internalError();
  }
  return result;
}

ExpansionStage SearchPipelineService::expand(
    const SearchRequest& request,
    std::vector<WeightedTerm>& weightedTerms) const {
  const auto started = Clock::now();
  ExpansionStage stage;
  std::vector<std::string> llmTerms;
  const bool hasOriginalTokens = !originalQueryTokens(request.query).empty();
  if (!request.expand || !hasOriginalTokens) {
    stage.source = ExpansionSource::kDisabled;  // Ollama is not called
  } else {
    stage.source = ExpansionSource::kFallback;
    ExpansionSlot slot(expansionsInFlight_);
    if (slot.held()) {  // otherwise: shed load, fall back at once
      if (auto terms = ollama_->expandQuery(request.query)) {
        llmTerms = std::move(*terms);
        stage.source = ExpansionSource::kLlm;
      }
    }
  }
  weightedTerms = buildQueryTerms(request.query, llmTerms);
  stage.expandedTerms.reserve(weightedTerms.size());
  for (const auto& term : weightedTerms)
    stage.expandedTerms.push_back(term.term);
  stage.timeMs = elapsedMs(started);
  return stage;
}

SearchStage SearchPipelineService::runSearch(
    const InvertedIndex& index, const std::vector<WeightedTerm>& weightedTerms,
    const std::vector<std::string>& expandedTerms, int limit) const {
  const auto started = Clock::now();
  SearchOutput output =
      index.search(weightedTerms, static_cast<std::size_t>(limit));
  const std::filesystem::path root(config_.datasetPath);
  for (auto& result : output.results) {
    result.snippet = makeSnippetFromFile(root / result.docId, expandedTerms);
  }
  SearchStage stage;
  stage.results = std::move(output.results);
  stage.totalMatches = output.totalMatches;
  stage.timeMs = elapsedMs(started);
  return stage;
}

std::variant<SearchResponse, ApiError>
SearchPipelineService::performKeywordSearch(const SearchRequest& request) {
  const auto index = snapshot();
  if (!index) return indexNotBuilt();
  try {
    SearchResponse response;
    std::vector<WeightedTerm> weighted;
    response.expansion = expand(request, weighted);
    response.search = runSearch(
        *index, weighted, response.expansion.expandedTerms, request.limit);
    return response;
  } catch (...) {
    return internalError();
  }
}

void SearchPipelineService::performStreamingSearch(const SearchRequest& request,
                                                   const MessageSink& sink) {
  const auto index = snapshot();
  if (!index) {
    sink(dumpJson(webSocketErrorJson(indexNotBuilt())));
    return;
  }

  std::vector<WeightedTerm> weighted;
  ExpansionStage expansion;
  try {
    expansion = expand(request, weighted);
  } catch (...) {
    sink(dumpJson(webSocketErrorJson(internalError())));
    return;
  }
  // Sent before the keyword search starts (§10.4).
  const json expansionMessage = {
      {"stage", "query_expansion"},
      {"data",
       {{"original", request.query},
        {"expanded_terms", expansion.expandedTerms},
        {"expansion", expansionSourceName(expansion.source)},
        {"time_ms", expansion.timeMs}}}};
  if (!sink(dumpJson(expansionMessage))) return;

  try {
    const SearchStage stage =
        runSearch(*index, weighted, expansion.expandedTerms, request.limit);
    const json searchMessage = {{"stage", "keyword_search"},
                                {"data",
                                 {{"results", resultsToJson(stage.results)},
                                  {"time_ms", stage.timeMs},
                                  {"total_matches", stage.totalMatches}}}};
    if (!sink(dumpJson(searchMessage))) return;
    sink(dumpJson(json{{"stage", "done"}}));
  } catch (...) {
    sink(dumpJson(webSocketErrorJson(internalError())));
  }
}

// --- WebSocket queueing
// --------------------------------------------------------

void SearchPipelineService::handleWebSocketMessage(
    const std::shared_ptr<WebSocketSession>& session, std::string payload,
    bool isBinary) {
  // Fast path: an invalid frame on an idle session is answered right here.
  // Frames of one connection arrive serially on its I/O thread and nothing is
  // running or queued, so no other message of this session can be reordered.
  bool idle = false;
  {
    std::lock_guard<std::mutex> lock(session->mutex_);
    if (session->closed_) return;
    idle = !session->draining_ && session->queue_.empty();
  }
  if (idle) {
    if (auto error = validationError(payload, isBinary)) {
      session->send(dumpJson(webSocketErrorJson(*error)));
      return;
    }
  }
  bool schedule = false;
  {
    std::lock_guard<std::mutex> lock(session->mutex_);
    if (session->closed_) return;
    session->queue_.push_back({std::move(payload), isBinary});
    if (!session->draining_) {
      session->draining_ = true;
      schedule = true;
    }
  }
  if (schedule) scheduleDrain(session);
}

void SearchPipelineService::scheduleDrain(
    const std::shared_ptr<WebSocketSession>& session) {
  try {
    webSocketPool_.enqueueTask([this, session] { drainOne(session); });
  } catch (...) {  // pool already shut down (server stopping)
    std::lock_guard<std::mutex> lock(session->mutex_);
    session->draining_ = false;
    session->queue_.clear();
  }
}

void SearchPipelineService::drainOne(
    const std::shared_ptr<WebSocketSession>& session) {
  WebSocketSession::Message message;
  {
    std::lock_guard<std::mutex> lock(session->mutex_);
    if (session->closed_ || session->queue_.empty() || stopping_.load()) {
      session->draining_ = false;
      session->queue_.clear();
      return;
    }
    message = std::move(session->queue_.front());
    session->queue_.pop_front();
  }
  try {
    processFrame(session, message.payload, message.isBinary);
  } catch (...) {
    // A failing query must not kill the worker; best effort error message.
    session->send(dumpJson(webSocketErrorJson(internalError())));
  }
  bool more = false;
  {
    std::lock_guard<std::mutex> lock(session->mutex_);
    more = !session->closed_ && !session->queue_.empty();
    if (!more) session->draining_ = false;
  }
  // Re-queue instead of looping so one chatty connection cannot monopolise a
  // worker while other connections wait.
  if (more) scheduleDrain(session);
}

void SearchPipelineService::processFrame(
    const std::shared_ptr<WebSocketSession>& session,
    const std::string& payload, bool isBinary) {
  const auto send = [&session](const std::string& text) {
    return session->send(text);
  };
  if (isBinary) {
    send(dumpJson(webSocketErrorJson(binaryFrameError())));
    return;
  }
  auto parsed = parseSearchRequest(payload, MessageSource::kWebSocketFrame);
  if (const auto* error = std::get_if<ApiError>(&parsed)) {
    send(dumpJson(webSocketErrorJson(*error)));
    return;
  }
  performStreamingSearch(std::get<SearchRequest>(parsed), send);
}

ServiceStatus SearchPipelineService::status() const {
  ServiceStatus status;
  const auto index = snapshot();
  status.indexing = building_.load();
  status.indexReady = index != nullptr;
  if (index) status.stats = index->getStats();
  status.stats.indexReady = status.indexReady;
  status.stats.indexing = status.indexing;
  return status;
}

bool SearchPipelineService::ollamaHealthy() { return ollama_->checkHealth(); }

}  // namespace search
