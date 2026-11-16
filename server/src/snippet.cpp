#include "search/snippet.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>

#include "search/tokenizer.h"

namespace search {

namespace {

constexpr char32_t kReplacement = 0xFFFD;
constexpr char32_t kBom = 0xFEFF;

// The match is only recognised when its token ends, at most kMaxTokenLength
// code points after its start, and the window begins kSnippetContextBefore
// code points before the start. The ring of recent code points of T must
// therefore hold at least 60 + 64 entries; 256 keeps the index a cheap mask.
constexpr std::size_t kRingSize = 256;
static_assert(kRingSize >= kSnippetContextBefore + kMaxTokenLength);
static_assert((kRingSize & (kRingSize - 1)) == 0);

bool isSnippetSpace(char32_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

void appendUtf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Expanded terms as a sorted vector of views plus a bit mask of the term
// lengths present: most tokens are rejected by one AND, the rest by a binary
// search (O(log n) for up to 48 terms, no hashing, no per-call node
// allocations).
class TermSet {
 public:
  explicit TermSet(const std::vector<std::string>& terms) {
    sorted_.reserve(terms.size());
    for (const std::string& t : terms) {
      if (t.size() < kMinTokenLength || t.size() > kMaxTokenLength) continue;
      sorted_.emplace_back(t);
      lengthMask_ |= std::uint64_t{1} << (t.size() - 1);
    }
    std::sort(sorted_.begin(), sorted_.end());
  }

  bool contains(std::string_view token) const {
    if ((lengthMask_ & (std::uint64_t{1} << (token.size() - 1))) == 0) {
      return false;
    }
    return std::binary_search(sorted_.begin(), sorted_.end(), token);
  }

 private:
  std::vector<std::string_view> sorted_;
  std::uint64_t lengthMask_ = 0;  // bit (len - 1) set if a term has len
};

// Streaming implementation of §4.6. Feed raw bytes with feed() (any split),
// then call finish() once.
//
//   bytes --decoder--> code points --BOM/whitespace--> T code points
//         --> token tracking + ring of recent T code points --> window
class SnippetScanner {
 public:
  explicit SnippetScanner(const TermSet& terms) : terms_(terms) {
    head_.reserve(kSnippetWindow);
  }

  // Returns true once the result is fixed; further input is not needed.
  bool feed(std::string_view bytes) {
    const auto* s = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    for (std::size_t i = 0; i < n && !done_; ++i) {
      const unsigned char b = s[i];
      if (need_ == 0 && !found_ && !first_ && !pendingSpace_ && b < 0x80) {
        // Fast path for the dominant case (a token character while searching
        // for the match): the same effects as onCodePoint(b), inlined.
        const char lower = detail::kLowerTable.map[b];
        if (lower != '\0') {
          if (tokenLength_ == 0) tokenStart_ = textLength_;
          if (tokenLength_ < kMaxTokenLength) token_[tokenLength_] = lower;
          ++tokenLength_;
          ring_[textLength_ & (kRingSize - 1)] = b;
          if (textLength_ < kSnippetWindow) head_.push_back(b);
          ++textLength_;
          continue;
        }
      }
      if (need_ > 0) {
        if (b >= lo_ && b <= hi_) {
          cp_ = (cp_ << 6) | (b & 0x3F);
          lo_ = 0x80;
          hi_ = 0xBF;
          if (--need_ == 0) onCodePoint(cp_);
          continue;
        }
        // Maximal invalid subpart ends here; re-examine `b` as a fresh byte.
        need_ = 0;
        onCodePoint(kReplacement);
        if (done_) break;
      }
      startSequence(b);
    }
    return done_;
  }

  std::string finish() {
    if (!done_ && need_ > 0) {  // truncated sequence at EOF
      need_ = 0;
      onCodePoint(kReplacement);
    }
    if (!done_ && !found_ && tokenLength_ > 0) {
      endToken();
      if (found_) startWindow();
    }
    std::string out;
    if (found_) {
      out.reserve(window_.size() * 3 + 6);
      if (windowStart_ > 0) out += "...";
      for (char32_t c : window_) appendUtf8(out, c);
      if (more_) out += "...";
    } else {
      out.reserve(head_.size() * 3 + 3);
      for (char32_t c : head_) appendUtf8(out, c);
      if (textLength_ > kSnippetWindow) out += "...";
    }
    return out;
  }

 private:
  void startSequence(unsigned char b) {
    if (b < 0x80) {
      onCodePoint(b);
    } else if (b >= 0xC2 && b <= 0xDF) {
      need_ = 1;
      cp_ = b & 0x1F;
      lo_ = 0x80;
      hi_ = 0xBF;
    } else if (b >= 0xE0 && b <= 0xEF) {
      need_ = 2;
      cp_ = b & 0x0F;
      lo_ = (b == 0xE0) ? 0xA0 : 0x80;
      hi_ = (b == 0xED) ? 0x9F : 0xBF;
    } else if (b >= 0xF0 && b <= 0xF4) {
      need_ = 3;
      cp_ = b & 0x07;
      lo_ = (b == 0xF0) ? 0x90 : 0x80;
      hi_ = (b == 0xF4) ? 0x8F : 0xBF;
    } else {
      onCodePoint(kReplacement);
    }
  }

  // Decoded code point -> BOM removal and whitespace normalisation.
  void onCodePoint(char32_t c) {
    if (first_) {
      first_ = false;
      // U+FEFF as the first code point can only come from bytes EF BB BF at
      // offset 0, i.e. exactly the BOM that §4.6 removes.
      if (c == kBom) return;
    }
    if (isSnippetSpace(c)) {
      pendingSpace_ = true;
      return;
    }
    if (pendingSpace_) {
      pendingSpace_ = false;
      if (textLength_ > 0) {  // leading whitespace is trimmed
        onTextCodePoint(U' ');
        if (done_) return;
      }
    }
    onTextCodePoint(c);
  }

  // One code point of T at position textLength_.
  void onTextCodePoint(char32_t c) {
    if (!found_) {
      const char lower = c < 0x80 ? detail::kLowerTable.map[c] : '\0';
      if (lower != '\0') {
        if (tokenLength_ == 0) tokenStart_ = textLength_;
        if (tokenLength_ < kMaxTokenLength) token_[tokenLength_] = lower;
        ++tokenLength_;
      } else if (tokenLength_ > 0) {
        endToken();
        if (found_) startWindow();
      }
    }
    if (found_) {
      if (window_.size() < kSnippetWindow) {
        window_.push_back(c);
      } else {
        more_ = true;  // T continues after the window
        done_ = true;
      }
    } else {
      ring_[textLength_ & (kRingSize - 1)] = c;
      if (textLength_ < kSnippetWindow) head_.push_back(c);
    }
    ++textLength_;
  }

  void endToken() {
    const std::size_t length = tokenLength_;
    tokenLength_ = 0;
    if (length < kMinTokenLength || length > kMaxTokenLength) return;
    const std::string_view token(token_.data(), length);
    if (isStopWord(token) || !terms_.contains(token)) return;
    found_ = true;
    windowStart_ = tokenStart_ > kSnippetContextBefore
                       ? tokenStart_ - kSnippetContextBefore
                       : 0;
  }

  // Copies T[windowStart_, textLength_) from the ring into the window. The
  // span is at most 60 + 64 code points, so it is still in the ring.
  void startWindow() {
    window_.reserve(kSnippetWindow);
    for (std::size_t k = windowStart_; k < textLength_; ++k) {
      window_.push_back(ring_[k & (kRingSize - 1)]);
    }
  }

  const TermSet& terms_;

  // UTF-8 decoder.
  int need_ = 0;                         // continuation bytes still expected
  unsigned char lo_ = 0x80, hi_ = 0xBF;  // allowed range of the next byte
  char32_t cp_ = 0;
  bool first_ = true;  // next decoded code point is the first of the file

  // Whitespace normaliser.
  bool pendingSpace_ = false;
  std::size_t textLength_ = 0;  // code points of T emitted so far

  // Current token run (lower-cased; only the first 64 chars are stored).
  std::array<char, kMaxTokenLength> token_{};
  std::size_t tokenLength_ = 0;
  std::size_t tokenStart_ = 0;

  // Before a match: recent code points of T and the first 200 (no-match
  // result).
  std::array<char32_t, kRingSize> ring_{};
  std::u32string head_;

  // After a match.
  bool found_ = false;
  std::size_t windowStart_ = 0;  // s
  std::u32string window_;        // T[s, s + 200)
  bool more_ = false;            // len(T) > s + 200
  bool done_ = false;
};

}  // namespace

std::string makeSnippet(std::string_view fileBytes,
                        const std::vector<std::string>& expandedTerms) {
  const TermSet terms(expandedTerms);
  SnippetScanner scanner(terms);
  scanner.feed(fileBytes);
  return scanner.finish();
}

std::string makeSnippetFromFile(const std::filesystem::path& path,
                                const std::vector<std::string>& expandedTerms) {
  return detail::makeSnippetFromFileChunked(path, expandedTerms,
                                            detail::kSnippetReadChunk);
}

namespace detail {

std::string makeSnippetChunked(std::string_view fileBytes,
                               const std::vector<std::string>& expandedTerms,
                               std::size_t chunkSize) {
  chunkSize = std::max<std::size_t>(chunkSize, 1);
  const TermSet terms(expandedTerms);
  SnippetScanner scanner(terms);
  while (!fileBytes.empty()) {
    const std::size_t n = std::min(chunkSize, fileBytes.size());
    if (scanner.feed(fileBytes.substr(0, n))) break;
    fileBytes.remove_prefix(n);
  }
  return scanner.finish();
}

std::string makeSnippetFromFileChunked(
    const std::filesystem::path& path,
    const std::vector<std::string>& expandedTerms, std::size_t chunkSize) {
  chunkSize = std::max<std::size_t>(chunkSize, 1);
  std::ifstream in;
  // Unbuffered stream: read() goes straight to the OS into our chunk buffer.
  in.rdbuf()->pubsetbuf(nullptr, 0);
  in.open(path, std::ios::binary);
  if (!in) return "";
  const TermSet terms(expandedTerms);
  SnippetScanner scanner(terms);
  std::string buffer(chunkSize, '\0');
  while (true) {
    in.read(buffer.data(), static_cast<std::streamsize>(chunkSize));
    if (in.bad()) return "";
    const auto got = static_cast<std::size_t>(in.gcount());
    if (got > 0 && scanner.feed(std::string_view(buffer.data(), got))) break;
    if (got < chunkSize) break;  // EOF
  }
  return scanner.finish();
}

}  // namespace detail

}  // namespace search
