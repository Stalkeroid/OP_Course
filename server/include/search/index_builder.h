// IndexBuilder — parallel full build of an InvertedIndex (docs/API.md §7.3,
// §7.4, §11).
//
// The coordinating thread only walks the directory tree and enqueues one task
// per document into a fresh CustomThreadPool(threads); workers read the file
// and call InvertedIndex::addDocument (tokenise + merge). The coordinating
// thread never tokenises. The time covers traversal, reading, tokenising,
// insertion, pool shutdown and finalize() (steady_clock, truncated to ms).
//
// The returned index is complete and is never written again, so the server
// can publish it as std::shared_ptr<const InvertedIndex> and swap it
// atomically.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "search/inverted_index.h"

namespace search {

enum class BuildError {
  kNone,
  kDatasetNotFound,  // -> 500 dataset_not_found
  kDatasetEmpty,     // -> 500 dataset_empty
};

struct BuildOutcome {
  BuildError error = BuildError::kNone;
  std::string message;                   // human-readable, for `error`
  std::shared_ptr<InvertedIndex> index;  // non-null iff ok()
  IndexStats stats;                      // valid iff ok()

  bool ok() const { return error == BuildError::kNone; }
};

// Machine-readable code of §3.2 ("dataset_not_found", "dataset_empty"; "" for
// kNone).
const char* buildErrorCode(BuildError error);

class IndexBuilder {
 public:
  // Builds a brand-new index from all *.txt files under datasetPath using a
  // new CustomThreadPool(threads). threads must be >= 1
  // (std::invalid_argument otherwise). Unexpected failures (I/O errors on
  // traversal, bad_alloc, ...) propagate as exceptions -> internal_error.
  static BuildOutcome buildIndex(const std::filesystem::path& datasetPath,
                                 int threads);

  // doc_id per §11: relative generic path ("science/doc_1.txt").
  static std::string makeDocId(const std::filesystem::path& file,
                               const std::filesystem::path& root);
};

}  // namespace search
