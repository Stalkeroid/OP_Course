// Tokenizer — docs/API.md §4.1 and §4.2. One implementation for documents and
// queries.
//
// Hot path: forEachToken() scans the byte buffer once and lower-cases each
// token into a 64-byte stack buffer, so no heap allocation happens per token.
// The callback receives a std::string_view into that stack buffer, valid only
// during the call.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace search {

inline constexpr std::size_t kMinTokenLength = 2;
inline constexpr std::size_t kMaxTokenLength = 64;
inline constexpr std::size_t kMaxOriginalTerms = 32;   // §4.2 step 1
inline constexpr std::size_t kMaxExpansionTerms = 16;  // §4.2 step 2
inline constexpr double kOriginalTermWeight = 1.0;
inline constexpr double kExpansionTermWeight = 0.5;

// ASCII letter or digit (§4.1 rule 1). Every other byte is a separator.
constexpr bool isTokenChar(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

constexpr char toLowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

namespace detail {
// 0 for separator bytes, otherwise the lower-case ASCII character.
struct LowerTable {
  char map[256];
  constexpr LowerTable() : map() {
    for (int c = 0; c < 256; ++c) {
      map[c] = isTokenChar(static_cast<unsigned char>(c))
                   ? toLowerAscii(static_cast<char>(c))
                   : '\0';
    }
  }
};
inline constexpr LowerTable kLowerTable{};
}  // namespace detail

// True for the 33 stop words of §4.1 (input must already be lower-case).
bool isStopWord(std::string_view lowerToken);

// Calls onToken(std::string_view lowerToken, std::size_t byteOffset) for every
// token of `text` in order, after the length and stop-word filters.
// byteOffset is the position of the token's first byte in `text`.
template <typename Callback>
void forEachToken(std::string_view text, Callback&& onToken) {
  const char* map = detail::kLowerTable.map;
  char buffer[kMaxTokenLength];
  const std::size_t n = text.size();
  std::size_t i = 0;
  while (i < n) {
    while (i < n && map[static_cast<unsigned char>(text[i])] == '\0') ++i;
    const std::size_t start = i;
    std::size_t length = 0;
    // Lower-case while scanning; only the first 64 bytes are stored.
    for (char c;
         i < n && (c = map[static_cast<unsigned char>(text[i])]) != '\0';
         ++i, ++length) {
      if (length < kMaxTokenLength) buffer[length] = c;
    }
    if (length < kMinTokenLength || length > kMaxTokenLength) continue;
    const std::string_view token(buffer, length);
    if (isStopWord(token)) continue;
    onToken(token, start);
  }
}

// Convenience: all tokens of `text` in order (duplicates kept).
std::vector<std::string> tokenize(std::string_view text);

// A query term with its weight (§4.2 step 4).
struct WeightedTerm {
  std::string term;
  double weight = kOriginalTermWeight;
};

// §4.2: `expanded_terms` = O followed by L.
//   O = tokens of `query`, deduplicated (first occurrence), first 32.
//   L = tokens of every expansion term, deduplicated, minus O, first 16.
// Weights: 1.0 for O, 0.5 for L. Pass an empty `expansionTerms` when
// expansion is disabled or fell back.
std::vector<WeightedTerm> buildQueryTerms(
    std::string_view query, const std::vector<std::string>& expansionTerms);

// Only the O part of §4.2 (used to decide whether Ollama is called at all).
std::vector<std::string> originalQueryTokens(std::string_view query);

}  // namespace search
