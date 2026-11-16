// Snippet builder — docs/API.md §4.6. Pure functions, thread-safe.
//
// The implementation is a single-pass streaming state machine
// (UTF-8 decoder -> whitespace normaliser -> tokenizer/window collector). It
// never materialises the whole normalised text T and stops consuming input as
// soon as the result is fixed: the first matching token is found, the
// 200-code-point window after `s` is complete and at least one more code point
// of T is known to follow (that decides the trailing "..."). Memory per call
// is O(1) apart from the output. The no-match case (window from position 0)
// necessarily scans to the end of the input, because the absence of a match
// is only known at EOF; it still runs in constant memory.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace search {

inline constexpr std::size_t kSnippetContextBefore = 60;  // code points
inline constexpr std::size_t kSnippetWindow = 200;        // code points

// Builds the snippet for one document from its raw file bytes:
//   UTF-8 decode (invalid sequences -> U+FFFD, "maximal subpart" rule),
//   leading BOM removed, ASCII whitespace runs collapsed to one space and
//   trimmed, window of 200 code points starting 60 code points before the
//   first token that is in `expandedTerms`, "..." markers on cut sides.
// If no token of the text is in `expandedTerms` (e.g. the file changed after
// indexing), the window starts at position 0.
std::string makeSnippet(std::string_view fileBytes,
                        const std::vector<std::string>& expandedTerms);

// Same as makeSnippet on the file contents, but reads `path` incrementally in
// chunks and stops reading once the snippet is complete. Returns "" if the
// file cannot be opened or a read error occurs.
std::string makeSnippetFromFile(const std::filesystem::path& path,
                                const std::vector<std::string>& expandedTerms);

namespace detail {

// Read size used by makeSnippetFromFile.
inline constexpr std::size_t kSnippetReadChunk = 16 * 1024;

// Testing hooks: identical results to the public functions, but the input is
// fed to the state machine in pieces of `chunkSize` bytes (>= 1), so that
// every chunk-boundary case can be exercised.
std::string makeSnippetChunked(std::string_view fileBytes,
                               const std::vector<std::string>& expandedTerms,
                               std::size_t chunkSize);
std::string makeSnippetFromFileChunked(
    const std::filesystem::path& path,
    const std::vector<std::string>& expandedTerms, std::size_t chunkSize);

}  // namespace detail

}  // namespace search
