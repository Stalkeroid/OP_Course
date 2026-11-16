// Determinism (1 vs 8 threads) and reader/writer stress tests. Run under
// -DENABLE_TSAN=ON to check for data races.
#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "search/index_builder.h"
#include "search/inverted_index.h"
#include "search/snippet.h"
#include "test_util.h"

using search::IndexBuilder;
using search::InvertedIndex;
using search::WeightedTerm;
using search_test::TempDir;

TEST(DeterminismTest, OneVsEightThreadsGiveIdenticalResults) {
  TempDir dir;
  std::mt19937 rng(12345);
  for (int i = 0; i < 300; ++i) {
    const std::string folder = "f" + std::to_string(i % 7);
    dir.write(folder + "/doc_" + std::to_string(i) + ".txt",
              search_test::randomText(rng, 50 + static_cast<int>(rng() % 400)));
  }
  // Exact duplicates to exercise the doc_id tie-break.
  dir.write("dup/a.txt", "climate climate ocean");
  dir.write("dup/b.txt", "climate climate ocean");

  auto one = IndexBuilder::buildIndex(dir.path(), 1);
  auto eight = IndexBuilder::buildIndex(dir.path(), 8);
  ASSERT_TRUE(one.ok());
  ASSERT_TRUE(eight.ok());
  EXPECT_EQ(one.stats.documents, eight.stats.documents);
  EXPECT_EQ(one.stats.uniqueTerms, eight.stats.uniqueTerms);
  EXPECT_EQ(eight.stats.threadsUsed, 8);

  const std::vector<std::vector<WeightedTerm>> queries = {
      {{"climate", 1.0}},
      {{"climate", 1.0}, {"change", 1.0}, {"warming", 0.5}},
      {{"ocean", 1.0}, {"river", 0.5}, {"water", 0.5}},
      {{"music", 1.0}, {"sport", 1.0}, {"travel", 1.0}, {"history", 0.5}},
      {{"nomatch", 1.0}},
  };
  for (const auto& q : queries) {
    for (std::size_t limit : {1u, 10u, 100u}) {
      auto a = one.index->search(q, limit);
      auto b = eight.index->search(q, limit);
      EXPECT_EQ(a.totalMatches, b.totalMatches);
      ASSERT_EQ(a.results.size(), b.results.size());
      for (std::size_t i = 0; i < a.results.size(); ++i) {
        EXPECT_EQ(a.results[i].docId, b.results[i].docId);
        EXPECT_EQ(a.results[i].score, b.results[i].score);  // bit-identical
      }
    }
  }
}

TEST(DeterminismTest, InsertionOrderDoesNotMatter) {
  std::mt19937 rng(7);
  std::vector<std::pair<std::string, std::string>> docs;
  for (int i = 0; i < 200; ++i) {
    docs.emplace_back("d" + std::to_string(i) + ".txt",
                      search_test::randomText(rng, 100));
  }
  InvertedIndex forward, backward;
  for (const auto& [id, text] : docs) forward.addDocument(id, text);
  for (auto it = docs.rbegin(); it != docs.rend(); ++it) {
    backward.addDocument(it->first, it->second);
  }
  const std::vector<WeightedTerm> q = {{"energy", 1.0}, {"solar", 0.5}};
  auto a = forward.search(q, 100);
  auto b = backward.search(q, 100);
  ASSERT_EQ(a.results.size(), b.results.size());
  for (std::size_t i = 0; i < a.results.size(); ++i) {
    EXPECT_EQ(a.results[i].docId, b.results[i].docId);
    EXPECT_EQ(a.results[i].score, b.results[i].score);
  }
}

TEST(StressTest, ConcurrentWritersAndReaders) {
  InvertedIndex index;
  constexpr int kWriters = 4;
  constexpr int kReaders = 4;
  constexpr int kDocsPerWriter = 300;
  std::atomic<int> writersDone{0};
  std::atomic<long> searches{0};
  std::atomic<bool> failed{false};

  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      std::mt19937 rng(100 + w);
      for (int i = 0; i < kDocsPerWriter; ++i) {
        // Every document contains "shared" so its df ends at the total.
        index.addDocument(
            "w" + std::to_string(w) + "/d" + std::to_string(i) + ".txt",
            "shared " + search_test::randomText(rng, 80));
      }
      writersDone.fetch_add(1);
    });
  }
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&, r] {
      const std::vector<std::vector<WeightedTerm>> qs = {
          {{"shared", 1.0}},
          {{"climate", 1.0}, {"ocean", 0.5}},
          {{"carbon", 1.0}, {"energy", 1.0}, {"policy", 0.5}}};
      std::size_t k = r;
      do {
        auto out = index.search(qs[k++ % qs.size()], 10);
        if (out.results.size() > 10 || out.results.size() > out.totalMatches) {
          failed.store(true);
        }
        for (std::size_t i = 0; i < out.results.size(); ++i) {
          const double s = out.results[i].score;
          if (!(s > 0.0 && s <= 1.0)) failed.store(true);
          if (i > 0 && s > out.results[i - 1].score) failed.store(true);
        }
        if (!out.results.empty() && out.results[0].score != 1.0)
          failed.store(true);
        (void)index.getStats();
        searches.fetch_add(1);
      } while (writersDone.load() < kWriters);
    });
  }
  for (auto& t : threads) t.join();

  EXPECT_FALSE(failed.load());
  EXPECT_GT(searches.load(), 0);
  const auto total = static_cast<std::uint64_t>(kWriters * kDocsPerWriter);
  EXPECT_EQ(index.getDocumentCount(), total);
  EXPECT_EQ(index.documentFrequency("shared"), total);
  auto out = index.search({{"shared", 1.0}}, 100);
  EXPECT_EQ(out.totalMatches, total);
  EXPECT_EQ(out.results.size(), 100u);
  // All documents tie on "shared" alone -> pure doc_id order.
  for (std::size_t i = 1; i < out.results.size(); ++i) {
    EXPECT_LT(out.results[i - 1].docId, out.results[i].docId);
  }
}

TEST(StressTest, SearchWhileBuildingAndSnippetsInParallel) {
  TempDir dir;
  std::mt19937 rng(99);
  for (int i = 0; i < 120; ++i) {
    dir.write("d" + std::to_string(i) + ".txt",
              search_test::randomText(rng, 200));
  }
  auto first = IndexBuilder::buildIndex(dir.path(), 2);
  ASSERT_TRUE(first.ok());
  std::shared_ptr<const InvertedIndex> served = first.index;

  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        auto out = served->search({{"climate", 1.0}, {"river", 0.5}}, 5);
        for (const auto& res : out.results) {
          auto s = search::makeSnippetFromFile(dir.path() / res.docId,
                                               {"climate", "river"});
          EXPECT_FALSE(s.empty());
        }
      }
    });
  }
  // Concurrent rebuilds while the old index serves searches.
  for (int i = 0; i < 3; ++i) {
    auto rebuilt = IndexBuilder::buildIndex(dir.path(), 4);
    ASSERT_TRUE(rebuilt.ok());
    EXPECT_EQ(rebuilt.stats.documents, 120u);
  }
  stop.store(true);
  for (auto& t : readers) t.join();
}
