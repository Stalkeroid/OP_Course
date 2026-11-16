// Snippet builder — docs/API.md §4.6. Pure functions, thread-safe.
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

// Reads `path` and calls makeSnippet. Returns "" if the file cannot be read.
std::string makeSnippetFromFile(const std::filesystem::path& path,
                                const std::vector<std::string>& expandedTerms);

}  // namespace search
