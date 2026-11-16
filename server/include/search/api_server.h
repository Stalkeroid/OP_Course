// ApiServer — HTTP + WebSocket front end (docs/API.md §6–§10, §13).
//
// The request handlers (handleHealthCheck, handleIndexPost, handleSearchPost,
// handleStats, handleStatic and the dispatcher handleRequest) take and return
// plain strings, so they are unit-tested without sockets. Crow is used only
// inside run() / handleWebSocket() (pimpl), the only code that needs a socket.
#pragma once

#include <memory>
#include <string>

#include "search/config.h"
#include "search/search_pipeline_service.h"

namespace search {

struct HttpReply {
  int status = 200;
  std::string contentType = "application/json";
  std::string body;
};

class ApiServer {
 public:
  ApiServer(Config config, std::shared_ptr<SearchPipelineService> service);
  ~ApiServer();

  ApiServer(const ApiServer&) = delete;
  ApiServer& operator=(const ApiServer&) = delete;

  // GET /api/health
  HttpReply handleHealthCheck();
  // POST /api/index
  HttpReply handleIndexPost(const std::string& body);
  // POST /api/search
  HttpReply handleSearchPost(const std::string& body);
  // GET /api/stats
  HttpReply handleStats();

  // Routes one HTTP request that is not a WebSocket upgrade: /api/* (JSON,
  // 404 not_found / 405 method_not_allowed in the error shape) and static
  // files (§13). `path` is the URL path without query string.
  HttpReply handleRequest(const std::string& method, const std::string& path,
                          const std::string& body);

  // GET /, GET /<path> from WEB_ROOT; plain-text 404 when missing.
  HttpReply handleStatic(const std::string& path);

  // Registers the /ws/search route (open / message / close callbacks) on the
  // Crow application. Called by run().
  void handleWebSocket();

  // Blocks: listens on 0.0.0.0:PORT with max(16, 2 x cores) worker threads
  // until stop() is called. Signal handling is left to the caller.
  void run();
  // Thread- and signal-safe request to leave run().
  void stop();

 private:
  struct Impl;

  Config config_;
  std::shared_ptr<SearchPipelineService> service_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace search
