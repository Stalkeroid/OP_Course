#include "search/index_builder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "test_util.h"

using search::BuildError;
using search::IndexBuilder;
using search_test::TempDir;

TEST(IndexBuilderTest, NestedFoldersNonTxtIgnoredEmptyFileCounted) {
  TempDir dir;
  dir.write("top.txt", "alpha beta");
  dir.write("science/physics/deep.txt", "alpha gamma gamma");
  dir.write("science/notes.md", "alpha alpha alpha");  // ignored
  dir.write("science/README", "alpha");                // ignored
  dir.write("science/upper.TXT", "alpha");             // ignored (case)
  dir.write("empty.txt", "");                          // counted, no terms
  dir.write(".hidden/secret.txt", "delta");            // hidden included

  auto outcome = IndexBuilder::buildIndex(dir.path(), 2);
  ASSERT_TRUE(outcome.ok()) << outcome.message;
  ASSERT_NE(outcome.index, nullptr);
  EXPECT_EQ(outcome.stats.documents, 4u);
  EXPECT_EQ(outcome.stats.threadsUsed, 2);
  EXPECT_EQ(outcome.stats.filesSkipped, 0u);
  EXPECT_GE(outcome.stats.indexBuildTimeMs, 0);
  EXPECT_EQ(outcome.stats.uniqueTerms, 4u);  // alpha beta gamma delta
  EXPECT_EQ(outcome.index->getStats().indexBuildTimeMs,
            outcome.stats.indexBuildTimeMs);

  auto out = outcome.index->search({{"alpha", 1.0}}, 10);
  ASSERT_EQ(out.results.size(), 2u);
  std::set<std::string> ids;
  for (const auto& r : out.results) ids.insert(r.docId);
  EXPECT_EQ(ids,
            (std::set<std::string>{"top.txt", "science/physics/deep.txt"}));

  auto hidden = outcome.index->search({{"delta", 1.0}}, 10);
  ASSERT_EQ(hidden.results.size(), 1u);
  EXPECT_EQ(hidden.results[0].docId, ".hidden/secret.txt");
}

TEST(IndexBuilderTest, MissingDirectory) {
  TempDir dir;
  auto outcome = IndexBuilder::buildIndex(dir.path() / "does_not_exist", 2);
  EXPECT_EQ(outcome.error, BuildError::kDatasetNotFound);
  EXPECT_STREQ(search::buildErrorCode(outcome.error), "dataset_not_found");
  EXPECT_EQ(outcome.index, nullptr);
}

TEST(IndexBuilderTest, PathIsAFile) {
  TempDir dir;
  dir.write("file.txt", "alpha");
  auto outcome = IndexBuilder::buildIndex(dir.path() / "file.txt", 1);
  EXPECT_EQ(outcome.error, BuildError::kDatasetNotFound);
}

TEST(IndexBuilderTest, EmptyDirectoryAndNoTxtFiles) {
  TempDir dir;
  auto empty = IndexBuilder::buildIndex(dir.path(), 2);
  EXPECT_EQ(empty.error, BuildError::kDatasetEmpty);
  EXPECT_STREQ(search::buildErrorCode(empty.error), "dataset_empty");

  dir.write("a/b/notes.md", "alpha");
  auto noTxt = IndexBuilder::buildIndex(dir.path(), 2);
  EXPECT_EQ(noTxt.error, BuildError::kDatasetEmpty);
  EXPECT_EQ(noTxt.index, nullptr);
}

TEST(IndexBuilderTest, UnreadableFileSkippedAndCounted) {
  if (::geteuid() == 0)
    GTEST_SKIP() << "root can read files without permission";
  TempDir dir;
  dir.write("ok.txt", "alpha");
  dir.write("locked.txt", "alpha");
  std::filesystem::permissions(dir.path() / "locked.txt",
                               std::filesystem::perms::none);
  auto outcome = IndexBuilder::buildIndex(dir.path(), 2);
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome.stats.documents, 1u);
  EXPECT_EQ(outcome.stats.filesSkipped, 1u);
}

TEST(IndexBuilderTest, SymlinksNotFollowed) {
  TempDir dir;
  TempDir outside;
  outside.write("x.txt", "alpha");
  dir.write("real.txt", "alpha");
  std::filesystem::create_directory_symlink(outside.path(),
                                            dir.path() / "linkdir");
  std::filesystem::create_symlink(outside.path() / "x.txt",
                                  dir.path() / "link.txt");
  auto outcome = IndexBuilder::buildIndex(dir.path(), 1);
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome.stats.documents, 1u);
}

TEST(IndexBuilderTest, DocIdIsRelativeGenericPath) {
  EXPECT_EQ(IndexBuilder::makeDocId("/data/set/a/b.txt", "/data/set"),
            "a/b.txt");
  EXPECT_EQ(IndexBuilder::makeDocId("/data/set/b.txt", "/data/set"), "b.txt");
}

TEST(IndexBuilderTest, RelativeDatasetPath) {
  TempDir dir;
  dir.write("sub/a.txt", "alpha");
  const auto cwd = std::filesystem::current_path();
  std::filesystem::current_path(dir.path());
  auto outcome = IndexBuilder::buildIndex("./sub/..", 1);
  std::filesystem::current_path(cwd);
  ASSERT_TRUE(outcome.ok());
  auto out = outcome.index->search({{"alpha", 1.0}}, 10);
  ASSERT_EQ(out.results.size(), 1u);
  EXPECT_EQ(out.results[0].docId, "sub/a.txt");
}

TEST(IndexBuilderTest, RejectsZeroThreads) {
  TempDir dir;
  EXPECT_THROW(IndexBuilder::buildIndex(dir.path(), 0), std::invalid_argument);
}
