// In-process fake Ollama (cpp-httplib server on a random local port) for the
// OllamaClient and pipeline tests.
#pragma once

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

namespace search_test {

class FakeOllama {
 public:
  FakeOllama() {
    server_.Get("/api/tags",
                [this](const httplib::Request&, httplib::Response& res) {
                  std::lock_guard<std::mutex> lock(mutex_);
                  res.set_content(tagsBody_, "application/json");
                });
    server_.Post("/api/generate", [this](const httplib::Request& req,
                                         httplib::Response& res) {
      std::string answer;
      int status = 200;
      int delay = 0;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        lastRequest_ = req.body;
        answer = generateBody_;
        status = generateStatus_;
        delay = delayMs_;
      }
      generateCalls_.fetch_add(1);
      if (delay > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
      res.status = status;
      res.set_content(answer, "application/json");
    });
    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
  }
  ~FakeOllama() {
    server_.stop();
    if (thread_.joinable()) thread_.join();
  }

  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }
  int port() const { return port_; }

  void setTags(std::string body) {
    std::lock_guard<std::mutex> lock(mutex_);
    tagsBody_ = std::move(body);
  }
  // `modelAnswer` is the text of the "response" field.
  void setModelAnswer(const std::string& modelAnswer) {
    std::lock_guard<std::mutex> lock(mutex_);
    generateBody_ = wrap(modelAnswer);
  }
  void setGenerateRaw(int status, std::string body) {
    std::lock_guard<std::mutex> lock(mutex_);
    generateStatus_ = status;
    generateBody_ = std::move(body);
  }
  void setDelayMs(int ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    delayMs_ = ms;
  }
  std::string lastRequest() {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastRequest_;
  }
  int generateCalls() const { return generateCalls_.load(); }

  static std::string wrap(const std::string& modelAnswer);

 private:
  httplib::Server server_;
  std::thread thread_;
  int port_ = 0;
  std::mutex mutex_;
  std::string tagsBody_ = R"({"models":[{"name":"llama3.2:1b"}]})";
  std::string generateBody_;
  std::string lastRequest_;
  int generateStatus_ = 200;
  int delayMs_ = 0;
  std::atomic<int> generateCalls_{0};
};

inline std::string FakeOllama::wrap(const std::string& modelAnswer) {
  return nlohmann::json{
      {"model", "llama3.2:1b"}, {"response", modelAnswer}, {"done", true}}
      .dump();
}

}  // namespace search_test
