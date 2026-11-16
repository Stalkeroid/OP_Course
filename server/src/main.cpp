// search_server entry point: configuration from the environment, signal
// handling, and the blocking HTTP/WebSocket server.
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <thread>

#include "search/api_server.h"
#include "search/config.h"
#include "search/search_pipeline_service.h"

int main() {
  // Block the termination signals in every thread (threads inherit the mask)
  // and handle them in one dedicated thread with sigwait: deterministic and
  // independent of Crow's own signal handling.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  signal(SIGPIPE, SIG_IGN);

  search::Config config;
  try {
    config = search::loadConfigFromEnvironment();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "configuration error: %s\n", e.what());
    return 2;
  }
  std::printf("search_server starting: %s workers=%u\n",
              search::describeConfig(config).c_str(),
              search::defaultWorkerThreads());
  std::fflush(stdout);

  try {
    auto service = std::make_shared<search::SearchPipelineService>(config);
    search::ApiServer server(config, service);

    std::thread signalThread([&server, signals] {
      int received = 0;
      sigwait(&signals, &received);
      std::printf("signal %d received, shutting down\n", received);
      std::fflush(stdout);
      // Handlers blocked in an index build or an Ollama call cannot be
      // interrupted; give the graceful stop a short grace period, then exit
      // so `docker stop` never waits for its 10 s kill timeout.
      std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::fprintf(stderr, "graceful shutdown timed out, exiting\n");
        std::_Exit(0);
      }).detach();
      server.stop();
    });
    signalThread.detach();

    server.run();
    std::printf("server stopped\n");
    std::fflush(stdout);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return 1;
  }
  return 0;
}
