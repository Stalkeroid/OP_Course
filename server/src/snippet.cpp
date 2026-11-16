#include "search/snippet.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <unordered_set>

#include "search/tokenizer.h"

namespace search {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

bool inRange(unsigned char b, unsigned char lo, unsigned char hi) {
  return b >= lo && b <= hi;
}

// UTF-8 -> code points. Each maximal invalid subpart becomes one U+FFFD
// (Unicode "best practice", same as WHATWG / ICU).
std::u32string decodeUtf8(std::string_view in) {
  std::u32string out;
  out.reserve(in.size());
  const auto* s = reinterpret_cast<const unsigned char*>(in.data());
  const std::size_t n = in.size();
  std::size_t i = 0;
  while (i < n) {
    const unsigned char b0 = s[i];
    if (b0 < 0x80) {
      out.push_back(b0);
      ++i;
      continue;
    }
    int need = 0;
    unsigned char lo = 0x80, hi = 0xBF;  // allowed range of the 2nd byte
    char32_t cp = 0;
    if (inRange(b0, 0xC2, 0xDF)) {
      need = 1;
      cp = b0 & 0x1F;
    } else if (inRange(b0, 0xE0, 0xEF)) {
      need = 2;
      cp = b0 & 0x0F;
      if (b0 == 0xE0) lo = 0xA0;
      if (b0 == 0xED) hi = 0x9F;
    } else if (inRange(b0, 0xF0, 0xF4)) {
      need = 3;
      cp = b0 & 0x07;
      if (b0 == 0xF0) lo = 0x90;
      if (b0 == 0xF4) hi = 0x8F;
    } else {
      out.push_back(kReplacement);
      ++i;
      continue;
    }
    std::size_t j = i + 1;
    bool ok = true;
    for (int k = 0; k < need; ++k, ++j) {
      const unsigned char lowB = (k == 0) ? lo : 0x80;
      const unsigned char highB = (k == 0) ? hi : 0xBF;
      if (j >= n || !inRange(s[j], lowB, highB)) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (s[j] & 0x3F);
    }
    out.push_back(ok ? cp : kReplacement);
    i = j;  // on failure: skip the valid prefix, re-examine the bad byte
  }
  return out;
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

bool isSnippetSpace(char32_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

bool isTokenCodePoint(char32_t c) {
  return c < 0x80 && isTokenChar(static_cast<unsigned char>(c));
}

}  // namespace

std::string makeSnippet(std::string_view fileBytes,
                        const std::vector<std::string>& expandedTerms) {
  // 1. BOM + decode.
  if (fileBytes.substr(0, 3) == "\xEF\xBB\xBF") fileBytes.remove_prefix(3);
  const std::u32string decoded = decodeUtf8(fileBytes);

  // 2. Collapse whitespace runs into one space, trim.
  std::u32string text;
  text.reserve(decoded.size());
  bool pendingSpace = false;
  for (char32_t c : decoded) {
    if (isSnippetSpace(c)) {
      pendingSpace = true;
      continue;
    }
    if (pendingSpace && !text.empty()) text.push_back(U' ');
    pendingSpace = false;
    text.push_back(c);
  }

  // 3. First token (§4.1 rules) that is one of the expanded terms.
  const std::unordered_set<std::string_view> wanted(expandedTerms.begin(),
                                                    expandedTerms.end());
  std::size_t p = 0;
  bool found = false;
  std::string token;
  const std::size_t len = text.size();
  for (std::size_t i = 0; i < len && !found;) {
    while (i < len && !isTokenCodePoint(text[i])) ++i;
    const std::size_t start = i;
    while (i < len && isTokenCodePoint(text[i])) ++i;
    const std::size_t tokenLength = i - start;
    if (tokenLength < kMinTokenLength || tokenLength > kMaxTokenLength) {
      continue;
    }
    token.clear();
    for (std::size_t k = start; k < i; ++k) {
      token.push_back(toLowerAscii(static_cast<char>(text[k])));
    }
    if (isStopWord(token)) continue;
    if (wanted.count(token) != 0) {
      p = start;
      found = true;
    }
  }

  // 4–5. Window and markers.
  const std::size_t s =
      p > kSnippetContextBefore ? p - kSnippetContextBefore : 0;
  const std::size_t e = std::min(len, s + kSnippetWindow);
  std::string snippet;
  snippet.reserve((e - s) + 6);
  if (s > 0) snippet += "...";
  for (std::size_t k = s; k < e; ++k) appendUtf8(snippet, text[k]);
  if (e < len) snippet += "...";
  return snippet;
}

std::string makeSnippetFromFile(const std::filesystem::path& path,
                                const std::vector<std::string>& expandedTerms) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::string bytes((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  if (in.bad()) return "";
  return makeSnippet(bytes, expandedTerms);
}

}  // namespace search
