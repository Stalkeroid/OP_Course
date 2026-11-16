#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "search/custom_thread_pool.h"

using search::CustomThreadPool;

TEST(CustomThreadPoolTest, RejectsZeroThreads) {
  EXPECT_THROW(CustomThreadPool(0), std::invalid_argument);
}

TEST(CustomThreadPoolTest, RunsAllTasks) {
  CustomThreadPool pool(4);
  std::atomic<int> counter{0};
  for (int i = 0; i < 10000; ++i) {
    pool.enqueueTask([&counter] { counter.fetch_add(1); });
  }
  pool.waitAll();
  EXPECT_EQ(counter.load(), 10000);
  EXPECT_EQ(pool.size(), 4);
}

TEST(CustomThreadPoolTest, WaitAllWithEmptyQueueReturns) {
  CustomThreadPool pool(2);
  pool.waitAll();
  SUCCEED();
}

TEST(CustomThreadPoolTest, WaitAllWorksWhenTasksArriveSlowly) {
  CustomThreadPool pool(2);
  std::atomic<int> counter{0};
  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 3; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      pool.enqueueTask([&counter] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        counter.fetch_add(1);
      });
    }
    pool.waitAll();
    EXPECT_EQ(counter.load(), (round + 1) * 3);
  }
}

TEST(CustomThreadPoolTest, ShutdownDrainsPendingTasks) {
  std::atomic<int> counter{0};
  CustomThreadPool pool(1);
  // The first task blocks the only worker so the others stay queued.
  std::atomic<bool> release{false};
  pool.enqueueTask([&] {
    while (!release.load()) std::this_thread::yield();
    counter.fetch_add(1);
  });
  for (int i = 0; i < 100; ++i) {
    pool.enqueueTask([&counter] { counter.fetch_add(1); });
  }
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    release.store(true);
  });
  pool.shutdown();  // drains: every queued task still runs
  releaser.join();
  EXPECT_EQ(counter.load(), 101);
  EXPECT_THROW(pool.enqueueTask([] {}), std::runtime_error);
  pool.shutdown();  // idempotent
}

TEST(CustomThreadPoolTest, DestructorDrainsAndJoins) {
  std::atomic<int> counter{0};
  {
    CustomThreadPool pool(3);
    for (int i = 0; i < 1000; ++i) {
      pool.enqueueTask([&counter] { counter.fetch_add(1); });
    }
  }
  EXPECT_EQ(counter.load(), 1000);
}

TEST(CustomThreadPoolTest, ExceptionDoesNotKillWorkerAndIsRethrown) {
  CustomThreadPool pool(1);  // a single worker must survive the throw
  std::atomic<int> counter{0};
  pool.enqueueTask([] { throw std::runtime_error("first"); });
  pool.enqueueTask([] { throw std::logic_error("second"); });
  for (int i = 0; i < 50; ++i) {
    pool.enqueueTask([&counter] { counter.fetch_add(1); });
  }
  try {
    pool.waitAll();
    FAIL() << "waitAll should rethrow";
  } catch (const std::runtime_error& e) {
    EXPECT_STREQ(e.what(), "first");
  }
  EXPECT_EQ(counter.load(), 50);
  // The error is reported once; the pool keeps working.
  pool.enqueueTask([&counter] { counter.fetch_add(1); });
  EXPECT_NO_THROW(pool.waitAll());
  EXPECT_EQ(counter.load(), 51);
}

TEST(CustomThreadPoolTest, TasksMayEnqueueTasks) {
  CustomThreadPool pool(2);
  std::atomic<int> counter{0};
  for (int i = 0; i < 10; ++i) {
    pool.enqueueTask([&] {
      counter.fetch_add(1);
      pool.enqueueTask([&counter] { counter.fetch_add(1); });
    });
  }
  pool.waitAll();
  EXPECT_EQ(counter.load(), 20);
}
