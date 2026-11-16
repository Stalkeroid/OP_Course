#include "search/index_builder.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "search/custom_thread_pool.h"

namespace search {

namespace fs = std::filesystem;

namespace {

bool hasTxtSuffix(const fs::path& file) {
  const std::string name = file.filename().string();
  return name.size() >= 4 && name.compare(name.size() - 4, 4, ".txt") == 0;
}

// Reads the whole file into `buffer` (reused per worker thread).
bool readFile(const fs::path& path, std::string& buffer) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec) return false;
  buffer.resize(static_cast<std::size_t>(size));
  in.read(buffer.data(), static_cast<std::streamsize>(size));
  if (in.bad()) return false;
  buffer.resize(static_cast<std::size_t>(in.gcount()));
  return true;
}

}  // namespace

const char* buildErrorCode(BuildError error) {
  switch (error) {
    case BuildError::kDatasetNotFound:
      return "dataset_not_found";
    case BuildError::kDatasetEmpty:
      return "dataset_empty";
    case BuildError::kNone:
      break;
  }
  return "";
}

std::string IndexBuilder::makeDocId(const fs::path& file,
                                    const fs::path& root) {
  return fs::relative(file, root).generic_string();
}

BuildOutcome IndexBuilder::buildIndex(const fs::path& datasetPath,
                                      int threads) {
  if (threads < 1) {
    throw std::invalid_argument("buildIndex: threads must be >= 1");
  }
  const auto started = std::chrono::steady_clock::now();
  BuildOutcome outcome;

  // Dataset checks: must exist, be a directory and be listable.
  std::error_code ec;
  if (!fs::is_directory(datasetPath, ec)) {
    outcome.error = BuildError::kDatasetNotFound;
    outcome.message = "dataset path does not exist or is not a directory: " +
                      datasetPath.string();
    return outcome;
  }
  fs::directory_iterator probe(datasetPath, ec);
  if (ec) {
    outcome.error = BuildError::kDatasetNotFound;
    outcome.message =
        "dataset directory is not readable: " + datasetPath.string() + " (" +
        ec.message() + ")";
    return outcome;
  }
  // relative() needs a canonical-looking root; resolve once.
  const fs::path root = fs::absolute(datasetPath).lexically_normal();

  auto index = std::make_shared<InvertedIndex>();
  std::atomic<std::uint64_t> filesSkipped{0};
  {
    CustomThreadPool pool(threads);
    // Symbolic links are not followed (default options); unreadable
    // subdirectories are skipped.
    const auto options = fs::directory_options::skip_permission_denied;
    for (auto it = fs::recursive_directory_iterator(root, options);
         it != fs::recursive_directory_iterator(); ++it) {
      const fs::directory_entry& entry = *it;
      // symlink_status: a symlink to a file is not a regular file here.
      if (!entry.is_regular_file() || entry.is_symlink()) continue;
      if (!hasTxtSuffix(entry.path())) continue;
      fs::path file = entry.path();
      std::string docId = makeDocId(file, root);
      pool.enqueueTask([index, &filesSkipped, file = std::move(file),
                        docId = std::move(docId)]() mutable {
        thread_local std::string buffer;
        if (!readFile(file, buffer)) {
          filesSkipped.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        index->addDocument(std::move(docId), buffer);
        if (buffer.capacity() > (std::size_t{8} << 20)) {
          std::string().swap(buffer);  // do not pin huge buffers
        }
      });
    }
    pool.waitAll();  // rethrows the first worker exception
    pool.shutdown();
  }
  index->finalize();

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  if (index->getDocumentCount() == 0) {
    outcome.error = BuildError::kDatasetEmpty;
    outcome.message =
        "dataset contains no readable .txt files: " + datasetPath.string();
    return outcome;
  }

  index->setBuildInfo(elapsed.count(), threads, filesSkipped.load());
  outcome.stats = index->getStats();
  outcome.index = std::move(index);
  return outcome;
}

}  // namespace search
