// SearchPipelineService — index lifecycle and the search pipeline
// (docs/API.md §4, §5, §7, §8, §10). No HTTP types in here.
//
// Concurrency design:
//   * The served index is a std::shared_ptr<const InvertedIndex> that is
//     replaced under a tiny mutex (indexMutex_). A search copies the pointer
//     and works on that snapshot, so searches never wait for a rebuild and
//     there is no lock around searching itself.
//   * One build at a time: an atomic flag; the loser gets 409.
//   * WebSocket queries run on a CustomThreadPool owned by this class, never
//     on Crow's I/O threads. Every connection has a WebSocketSession with a
//     FIFO queue; at most one drain task per session is scheduled at a time,
//     so messages of different queries never interleave, while different
//     sessions run in parallel. A session is shared_ptr-owned by every task
//     that uses it and has a `closed` flag guarded by the same mutex that
//     serialises sends, so nothing is sent on (or freed under) a dead
//     connection.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "search/config.h"
#include "search/custom_thread_pool.h"
#include "search/index_builder.h"
#include "search/inverted_index.h"
#include "search/ollama_client.h"
#include "search/request_validation.h"

namespace search {

enum class ExpansionSource { kLlm, kFallback, kDisabled };
const char* expansionSourceName(ExpansionSource source);

// {doc_id, score, snippet} array of §8.2.
nlohmann::json resultsToJson(const std::vector<SearchResult>& results);

// Outcome of POST /api/index.
struct BuildResult {
  std::optional<ApiError> error;  // set on 409 / 500
  IndexStats stats;               // valid iff !error
};

// Snapshot of the server state for /api/health and /api/stats.
struct ServiceStatus {
  bool indexReady = false;
  bool indexing = false;
  IndexStats stats;  // zeros before the first build
};

// Everything the REST handler and the WebSocket stages need.
struct ExpansionStage {
  std::vector<std::string> expandedTerms;  // O followed by L (§4.2)
  ExpansionSource source = ExpansionSource::kDisabled;
  std::int64_t timeMs = 0;
};

struct SearchStage {
  std::vector<SearchResult> results;
  std::uint64_t totalMatches = 0;
  std::int64_t timeMs = 0;  // lookup + scoring + ranking + snippets
};

struct SearchResponse {
  ExpansionStage expansion;
  SearchStage search;
};

// Receives one WebSocket text message; returns false when the connection is
// gone (the pipeline then stops and discards the rest of its output).
using MessageSink = std::function<bool(const std::string&)>;

// Per-connection state of /ws/search (see the file comment).
class WebSocketSession {
 public:
  explicit WebSocketSession(std::function<void(std::string)> sendText);

  // Sends one text frame unless the session is closed. Returns false when
  // closed. Serialised with close(): after close() returned, sendText is never
  // invoked again.
  bool send(const std::string& message);
  // Marks the session closed and drops queued messages. Idempotent.
  void close();
  bool isClosed() const;

 private:
  friend class SearchPipelineService;
  struct Message {
    std::string payload;
    bool isBinary = false;
  };

  mutable std::mutex mutex_;
  std::function<void(std::string)> sendText_;
  std::deque<Message> queue_;
  bool closed_ = false;
  bool draining_ = false;  // a drain task is scheduled or running
};

// Builds a new index (replaceable in tests).
using IndexBuildFunction = std::function<BuildOutcome(
    const std::filesystem::path& datasetPath, int threads)>;

struct PipelineOptions {
  std::shared_ptr<OllamaClient> ollama;  // default: built from Config
  IndexBuildFunction builder;            // default: IndexBuilder::buildIndex
  int webSocketWorkers = 0;              // 0 -> defaultWorkerThreads()
};

class SearchPipelineService {
 public:
  explicit SearchPipelineService(Config config, PipelineOptions options = {});
  ~SearchPipelineService();

  SearchPipelineService(const SearchPipelineService&) = delete;
  SearchPipelineService& operator=(const SearchPipelineService&) = delete;

  // POST /api/index: full rebuild with `threads` workers, then atomic swap.
  // Never throws: failures are reported in BuildResult::error.
  BuildResult buildIndex(int threads);

  // POST /api/search: expand -> search. index_not_built -> ApiError (503);
  // unexpected failures -> internal_error (500).
  std::variant<SearchResponse, ApiError> performKeywordSearch(
      const SearchRequest& request);

  // WebSocket pipeline for one validated query: query_expansion ->
  // keyword_search -> done, or error (docs/API.md §10.4). Runs on the calling
  // thread; every message goes through `sink`.
  void performStreamingSearch(const SearchRequest& request,
                              const MessageSink& sink);

  // Entry point for a WebSocket frame: queues it on the session and makes sure
  // a pool worker drains the session. Never blocks and never runs the pipeline
  // on the calling (I/O) thread.
  void handleWebSocketMessage(const std::shared_ptr<WebSocketSession>& session,
                              std::string payload, bool isBinary);

  ServiceStatus status() const;
  bool ollamaHealthy();

 private:
  std::shared_ptr<const InvertedIndex> snapshot() const;
  ExpansionStage expand(const SearchRequest& request,
                        std::vector<WeightedTerm>& weightedTerms) const;
  SearchStage runSearch(const InvertedIndex& index,
                        const std::vector<WeightedTerm>& weightedTerms,
                        const std::vector<std::string>& expandedTerms,
                        int limit) const;
  void scheduleDrain(const std::shared_ptr<WebSocketSession>& session);
  void drainOne(const std::shared_ptr<WebSocketSession>& session);
  void processFrame(const std::shared_ptr<WebSocketSession>& session,
                    const std::string& payload, bool isBinary);

  Config config_;
  std::shared_ptr<OllamaClient> ollama_;
  IndexBuildFunction builder_;

  mutable std::mutex indexMutex_;
  std::shared_ptr<const InvertedIndex> servedIndex_;  // guarded by indexMutex_
  std::atomic<bool> building_{false};
  std::atomic<bool> stopping_{false};

  // Declared last: destroyed first, so draining tasks still see valid members.
  CustomThreadPool webSocketPool_;
};

}  // namespace search
