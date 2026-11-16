#include "search/custom_thread_pool.h"

#include <stdexcept>
#include <utility>

namespace search {

CustomThreadPool::CustomThreadPool(int numThreads) {
  if (numThreads < 1) {
    throw std::invalid_argument("CustomThreadPool: numThreads must be >= 1");
  }
  workers.reserve(static_cast<std::size_t>(numThreads));
  try {
    for (int i = 0; i < numThreads; ++i) {
      workers.emplace_back([this] { workerLoop(); });
    }
  } catch (...) {
    shutdown();
    throw;
  }
}

CustomThreadPool::~CustomThreadPool() { shutdown(); }

void CustomThreadPool::enqueueTask(Task task) {
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (!isRunning) {
      throw std::runtime_error("CustomThreadPool: enqueueTask after shutdown");
    }
    taskQueue.push(std::move(task));
    ++pendingTasks;
  }
  conditionVar.notify_one();
}

void CustomThreadPool::waitAll() {
  std::unique_lock<std::mutex> lock(queueMutex);
  doneCondition.wait(lock, [this] { return pendingTasks == 0; });
  if (firstError) {
    std::exception_ptr error = std::exchange(firstError, nullptr);
    lock.unlock();
    std::rethrow_exception(error);
  }
}

void CustomThreadPool::shutdown() {
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    isRunning = false;
  }
  conditionVar.notify_all();
  for (std::thread& worker : workers) {
    if (worker.joinable()) worker.join();
  }
}

void CustomThreadPool::workerLoop() {
  for (;;) {
    Task task;
    {
      std::unique_lock<std::mutex> lock(queueMutex);
      conditionVar.wait(lock,
                        [this] { return !isRunning || !taskQueue.empty(); });
      // Drain semantics: exit only when stopping AND nothing is left.
      if (taskQueue.empty()) return;
      task = std::move(taskQueue.front());
      taskQueue.pop();
    }

    std::exception_ptr error;
    try {
      task();
    } catch (...) {
      error = std::current_exception();
    }
    task = nullptr;  // destroy captured state outside the lock

    bool allDone = false;
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      if (error && !firstError) firstError = error;
      allDone = (--pendingTasks == 0);
    }
    if (allDone) doneCondition.notify_all();
  }
}

}  // namespace search
