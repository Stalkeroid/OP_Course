// Helpers shared by the builder / concurrency tests.
#pragma once

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace search_test {

// A unique temporary directory removed on destruction.
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("search_core_test_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

  void write(const std::string& relative, const std::string& content) const {
    const auto file = path_ / relative;
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << content;
  }

 private:
  std::filesystem::path path_;
};

// Deterministic pseudo-random English-like text over a small vocabulary.
inline std::string randomText(std::mt19937& rng, int words) {
  static const char* kVocab[] = {
      "climate", "change",  "global",  "warming", "ocean",  "river",
      "energy",  "solar",   "wind",    "carbon",  "policy", "market",
      "science", "data",    "model",   "forest",  "city",   "water",
      "health",  "economy", "history", "music",   "sport",  "travel"};
  constexpr int kVocabSize = sizeof(kVocab) / sizeof(kVocab[0]);
  std::uniform_int_distribution<int> pick(0, kVocabSize - 1);
  std::string text;
  for (int i = 0; i < words; ++i) {
    text += kVocab[pick(rng)];
    text += (i % 13 == 12) ? ".\n" : " ";
  }
  return text;
}

}  // namespace search_test
