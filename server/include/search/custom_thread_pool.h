// CustomThreadPool — fixed-size pool of std::thread workers (own
// implementation).
//
// Locking scheme:
//   * One mutex (queueMutex) protects the task queue, the pending counter, the
//     isRunning flag and the first captured exception. The queue is touched
//     only for a push/pop, so the critical sections are tiny; the task itself
//     always runs with the mutex released.
//   * conditionVar wakes workers when a task is queued or the pool stops.
//     doneCondition wakes waitAll() when the pending counter drops to zero.
//     Every wait uses a predicate, so spurious wake-ups and notifications sent
//     before the wait started are never lost (the state, not the signal, is
//     checked under the mutex).
//
// Semantics:
//   * waitAll() blocks until every task enqueued so far has finished (queued +
//     running == 0). It then rethrows the first exception thrown by a task
//     since the previous waitAll(), if any. A throwing task never kills its
//     worker: the exception is caught and stored.
//   * shutdown() DRAINS: tasks already queued are still executed, then the
//     workers exit and are joined. enqueueTask() after shutdown() throws
//     std::runtime_error. shutdown() is idempotent; the destructor calls it.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace search {

class CustomThreadPool {
 public:
  using Task = std::function<void()>;

  // numThreads must be >= 1 (std::invalid_argument otherwise).
  explicit CustomThreadPool(int numThreads);
  ~CustomThreadPool();

  CustomThreadPool(const CustomThreadPool&) = delete;
  CustomThreadPool& operator=(const CustomThreadPool&) = delete;

  void enqueueTask(Task task);
  void waitAll();
  void shutdown();

  int size() const { return static_cast<int>(workers.size()); }

 private:
  void workerLoop();

  std::vector<std::thread> workers;
  std::queue<Task> taskQueue;
  std::mutex queueMutex;
  std::condition_variable conditionVar;   // task available or stopping
  std::condition_variable doneCondition;  // pendingTasks reached zero
  std::size_t pendingTasks = 0;           // queued + currently running
  std::exception_ptr firstError;
  bool isRunning = true;
};

}  // namespace search
