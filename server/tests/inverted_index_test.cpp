#include "search/inverted_index.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using search::InvertedIndex;
using search::WeightedTerm;

namespace {

// Tiny corpus with hand-computable scores (N = 3).
void fillCorpus(InvertedIndex& index) {
  index.addDocument("a.txt", "Apple apple banana");
  index.addDocument("b.txt", "apple, cherry");
  index.addDocument("c.txt", "cherry cherry CHERRY");
}

}  // namespace

TEST(InvertedIndexTest, CountsAndStats) {
  InvertedIndex index;
  fillCorpus(index);
  index.addDocument("empty.txt", "");
  EXPECT_EQ(index.getDocumentCount(), 4u);
  EXPECT_EQ(index.documentFrequency("apple"), 2u);
  EXPECT_EQ(index.documentFrequency("cherry"), 2u);
  EXPECT_EQ(index.documentFrequency("banana"), 1u);
  EXPECT_EQ(index.documentFrequency("durian"), 0u);
  auto stats = index.getStats();
  EXPECT_EQ(stats.documents, 4u);
  EXPECT_EQ(stats.uniqueTerms, 3u);
  EXPECT_TRUE(stats.indexReady);
}

TEST(InvertedIndexTest, ScoreFormulaSingleTerm) {
  InvertedIndex index;
  fillCorpus(index);
  auto out = index.search({WeightedTerm{"apple", 1.0}}, 10);
  ASSERT_EQ(out.totalMatches, 2u);
  ASSERT_EQ(out.results.size(), 2u);
  const double idf = std::log(1.0 + 3.0 / 2.0);
  const double rawA = (1.0 + std::log(2.0)) * idf;
  const double rawB = 1.0 * idf;
  EXPECT_EQ(out.results[0].docId, "a.txt");
  EXPECT_DOUBLE_EQ(out.results[0].score, 1.0);
  EXPECT_EQ(out.results[1].docId, "b.txt");
  EXPECT_DOUBLE_EQ(out.results[1].score, std::round(rawB / rawA * 1e6) / 1e6);
  EXPECT_DOUBLE_EQ(out.results[1].score, 0.590616);
  EXPECT_TRUE(out.results[0].snippet.empty());
}

TEST(InvertedIndexTest, WeightsOriginalVsExpansion) {
  InvertedIndex index;
  fillCorpus(index);
  // apple original (1.0), cherry expansion (0.5).
  auto out = index.search({{"apple", 1.0}, {"cherry", 0.5}}, 10);
  ASSERT_EQ(out.totalMatches, 3u);
  const double idf = std::log(2.5);  // both terms have df = 2
  const double rawA = (1 + std::log(2.0)) * idf;
  const double rawB = idf + 0.5 * idf;
  const double rawC = 0.5 * (1 + std::log(3.0)) * idf;
  ASSERT_EQ(out.results.size(), 3u);
  EXPECT_EQ(out.results[0].docId, "a.txt");
  EXPECT_EQ(out.results[1].docId, "b.txt");
  EXPECT_EQ(out.results[2].docId, "c.txt");
  EXPECT_DOUBLE_EQ(out.results[1].score, std::round(rawB / rawA * 1e6) / 1e6);
  EXPECT_DOUBLE_EQ(out.results[2].score, std::round(rawC / rawA * 1e6) / 1e6);

  // Same terms with swapped weights: c.txt (cherry x3) wins.
  auto swapped = index.search({{"apple", 0.5}, {"cherry", 1.0}}, 10);
  EXPECT_EQ(swapped.results[0].docId, "c.txt");
}

TEST(InvertedIndexTest, TieBrokenByDocIdAscending) {
  InvertedIndex index;
  index.addDocument("z.txt", "same words here");
  index.addDocument("m/b.txt", "same words here");
  index.addDocument("m/a.txt", "same words here");
  index.addDocument("other.txt", "unrelated");
  auto out = index.search({{"words", 1.0}}, 10);
  ASSERT_EQ(out.results.size(), 3u);
  EXPECT_EQ(out.results[0].docId, "m/a.txt");
  EXPECT_EQ(out.results[1].docId, "m/b.txt");
  EXPECT_EQ(out.results[2].docId, "z.txt");
  for (const auto& r : out.results) EXPECT_DOUBLE_EQ(r.score, 1.0);
}

TEST(InvertedIndexTest, LimitAndTotalMatches) {
  InvertedIndex index;
  for (int i = 0; i < 50; ++i) {
    std::string text;
    for (int k = 0; k <= i; ++k) text += "common ";
    index.addDocument("d" + std::to_string(100 + i) + ".txt", text);
  }
  auto out = index.search({{"common", 1.0}}, 5);
  EXPECT_EQ(out.totalMatches, 50u);
  ASSERT_EQ(out.results.size(), 5u);
  EXPECT_EQ(out.results[0].docId, "d149.txt");  // most occurrences
  EXPECT_EQ(out.results[4].docId, "d145.txt");
  for (std::size_t i = 1; i < out.results.size(); ++i) {
    EXPECT_LE(out.results[i].score, out.results[i - 1].score);
  }
  auto none = index.search({{"common", 1.0}}, 0);
  EXPECT_EQ(none.totalMatches, 50u);
  EXPECT_TRUE(none.results.empty());
}

TEST(InvertedIndexTest, NoMatchAndEmptyQuery) {
  InvertedIndex index;
  EXPECT_TRUE(index.search({{"apple", 1.0}}, 10).results.empty());  // N = 0
  fillCorpus(index);
  auto out = index.search({{"zzqxjv", 1.0}}, 10);
  EXPECT_EQ(out.totalMatches, 0u);
  EXPECT_TRUE(out.results.empty());
  EXPECT_TRUE(index.search(std::vector<WeightedTerm>{}, 10).results.empty());
}

TEST(InvertedIndexTest, DuplicateTermsCountOnce) {
  InvertedIndex index;
  fillCorpus(index);
  auto once = index.search({{"apple", 1.0}, {"banana", 1.0}}, 10);
  auto twice =
      index.search({{"apple", 1.0}, {"apple", 0.5}, {"banana", 1.0}}, 10);
  ASSERT_EQ(once.results.size(), twice.results.size());
  for (std::size_t i = 0; i < once.results.size(); ++i) {
    EXPECT_EQ(once.results[i].docId, twice.results[i].docId);
    EXPECT_DOUBLE_EQ(once.results[i].score, twice.results[i].score);
  }
}

TEST(InvertedIndexTest, ScoreRoundingAndClamping) {
  EXPECT_DOUBLE_EQ(search::roundScore(1.0), 1.0);
  EXPECT_DOUBLE_EQ(search::roundScore(0.1234564), 0.123456);
  EXPECT_DOUBLE_EQ(search::roundScore(0.1234566), 0.123457);
  EXPECT_DOUBLE_EQ(search::roundScore(1e-9), 0.000001);
}

TEST(InvertedIndexTest, TinyScoresClampedNotZero) {
  InvertedIndex index;
  std::string heavy;
  for (int i = 0; i < 1000; ++i) heavy += "rare ";
  index.addDocument("heavy.txt", heavy + "common");
  for (int i = 0; i < 5000; ++i) {
    index.addDocument("f" + std::to_string(i) + ".txt", "common filler");
  }
  auto out = index.search({{"rare", 1.0}, {"common", 0.5}}, 100);
  ASSERT_FALSE(out.results.empty());
  EXPECT_EQ(out.results[0].docId, "heavy.txt");
  for (const auto& r : out.results) {
    EXPECT_GT(r.score, 0.0);
    EXPECT_LE(r.score, 1.0);
  }
}
