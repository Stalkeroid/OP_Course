// Performance sanity tool (not built by default):
//   index_bench <dataset_dir> <threads> [query ...]
// Builds the index once and prints build time, document/term counts, peak
// RSS of the process, and the latency of a few searches.
#include <sys/resource.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "search/index_builder.h"
#include "search/snippet.h"
#include "search/tokenizer.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: index_bench <dataset_dir> <threads> [query ...]\n";
    return 2;
  }
  const std::string dataset = argv[1];
  const int threads = std::atoi(argv[2]);
  auto outcome = search::IndexBuilder::buildIndex(dataset, threads);
  if (!outcome.ok()) {
    std::cerr << search::buildErrorCode(outcome.error) << ": "
              << outcome.message << "\n";
    return 1;
  }
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  std::cout << "threads=" << outcome.stats.threadsUsed
            << " documents=" << outcome.stats.documents
            << " unique_terms=" << outcome.stats.uniqueTerms
            << " files_skipped=" << outcome.stats.filesSkipped
            << " time_ms=" << outcome.stats.indexBuildTimeMs
            << " peak_rss_mb=" << usage.ru_maxrss / 1024 << "\n";

  for (int i = 3; i < argc; ++i) {
    const auto terms = search::buildQueryTerms(argv[i], {});
    std::vector<std::string> expanded;
    for (const auto& t : terms) expanded.push_back(t.term);
    const auto start = std::chrono::steady_clock::now();
    auto out = outcome.index->search(terms, 10);
    for (auto& r : out.results) {
      r.snippet = search::makeSnippetFromFile(
          std::filesystem::path(dataset) / r.docId, expanded);
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    std::cout << "query='" << argv[i] << "' total_matches=" << out.totalMatches
              << " search_us=" << us;
    if (!out.results.empty()) {
      std::cout << " top=" << out.results[0].docId;
    }
    std::cout << "\n";
  }
  return 0;
}
