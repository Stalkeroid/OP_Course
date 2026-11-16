#include "search/tokenizer.h"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace search {

namespace {

// Sorted (byte-wise) so isStopWord can binary-search. Exactly §4.1, 33 words.
constexpr std::array<std::string_view, 33> kStopWords = {
    "an",   "and", "are",  "as",   "at",   "be",   "but", "by",    "for",
    "from", "has", "have", "he",   "her",  "his",  "in",  "is",    "it",
    "its",  "of",  "on",   "or",   "she",  "that", "the", "their", "they",
    "this", "to",  "was",  "were", "will", "with"};

constexpr bool isSorted() {
  for (std::size_t i = 1; i < kStopWords.size(); ++i) {
    if (!(kStopWords[i - 1] < kStopWords[i])) return false;
  }
  return true;
}
static_assert(isSorted(), "stop-word table must be sorted");

}  // namespace

bool isStopWord(std::string_view lowerToken) {
  // All stop words have 2..5 characters; cheap reject for the common case.
  if (lowerToken.size() > 5) return false;
  return std::binary_search(kStopWords.begin(), kStopWords.end(), lowerToken);
}

std::vector<std::string> tokenize(std::string_view text) {
  std::vector<std::string> tokens;
  forEachToken(text, [&tokens](std::string_view token, std::size_t) {
    tokens.emplace_back(token);
  });
  return tokens;
}

std::vector<std::string> originalQueryTokens(std::string_view query) {
  std::vector<std::string> result;
  std::unordered_set<std::string> seen;
  for (std::string& token : tokenize(query)) {
    if (result.size() == kMaxOriginalTerms) break;
    if (seen.insert(token).second) result.push_back(std::move(token));
  }
  return result;
}

std::vector<WeightedTerm> buildQueryTerms(
    std::string_view query, const std::vector<std::string>& expansionTerms) {
  std::vector<WeightedTerm> terms;
  std::unordered_set<std::string> seen;
  for (std::string& token : originalQueryTokens(query)) {
    seen.insert(token);
    terms.push_back({std::move(token), kOriginalTermWeight});
  }
  std::size_t added = 0;
  for (const std::string& raw : expansionTerms) {
    for (std::string& token : tokenize(raw)) {
      if (added == kMaxExpansionTerms) return terms;
      if (!seen.insert(token).second) continue;  // duplicate or already in O
      terms.push_back({std::move(token), kExpansionTermWeight});
      ++added;
    }
  }
  return terms;
}

}  // namespace search
