#include "search/snippet.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "search/tokenizer.h"

using search::makeSnippet;

namespace {

// Number of UTF-8 code points (input assumed valid).
std::size_t codePoints(const std::string& s) {
  std::size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

std::string words(int count, const std::string& word = "lorem") {
  std::string out;
  for (int i = 0; i < count; ++i) {
    if (i) out += ' ';
    out += word;
  }
  return out;
}

}  // namespace

TEST(SnippetTest, ShortTextNoMarkers) {
  EXPECT_EQ(makeSnippet("  Hello,\n\n\tworld!  ", {"world"}), "Hello, world!");
}

TEST(SnippetTest, MatchAtStart) {
  const std::string text = "target " + words(100);  // 7 + 599 = 606 cps
  const std::string s = makeSnippet(text, {"target"});
  EXPECT_EQ(s.substr(0, 6), "target");
  EXPECT_EQ(s.substr(s.size() - 3), "...");
  EXPECT_EQ(codePoints(s), 203u);
}

TEST(SnippetTest, MatchInMiddle) {
  const std::string prefix = std::string(100, 'x') + " ";  // 101 cps
  const std::string text = prefix + "needle " + std::string(300, 'y');
  const std::string s = makeSnippet(text, {"needle"});
  // p = 101, s = 41, e = 241.
  const std::string expected = "..." + text.substr(41, 200) + "...";
  EXPECT_EQ(s, expected);
  EXPECT_EQ(codePoints(s), 206u);
  EXPECT_NE(s.find("needle"), std::string::npos);
}

TEST(SnippetTest, MatchNearEnd) {
  const std::string text = std::string(300, 'x') + " needle end";
  const std::string s = makeSnippet(text, {"needle"});
  // p = 301, s = 241, e = len = 312 -> no trailing marker.
  EXPECT_EQ(s, "..." + text.substr(241));
}

TEST(SnippetTest, MatchWithin60NoLeadingMarker) {
  const std::string text =
      std::string(59, 'x') + " needle " + std::string(500, 'z');
  const std::string s = makeSnippet(text, {"needle"});
  EXPECT_EQ(s.substr(0, 3), "xxx");
  EXPECT_EQ(s, text.substr(0, 200) + "...");
}

TEST(SnippetTest, FirstMatchingTokenWinsAndStopWordsSkipped) {
  const std::string text = std::string(100, 'q') + " beta alpha";
  // "beta" occurs before "alpha" -> window around beta.
  const std::string s = makeSnippet(text, {"alpha", "beta"});
  EXPECT_EQ(s, "..." + text.substr(41));
  // A token must be a whole token: "alphabet" does not match "alpha".
  EXPECT_EQ(makeSnippet("alphabet", {"alpha"}), "alphabet");
}

TEST(SnippetTest, BomRemoved) {
  EXPECT_EQ(makeSnippet("\xEF\xBB\xBFHello there", {"hello"}), "Hello there");
}

TEST(SnippetTest, InvalidUtf8BecomesReplacement) {
  // 0xFF invalid; truncated 3-byte sequence E2 82 followed by 'a';
  // overlong C0 80; surrogate ED A0 80.
  const std::string s = makeSnippet(
      "ok \xFF x \xE2\x82"
      "a \xC0\x80 \xED\xA0\x80",
      {"ok"});
  EXPECT_EQ(s,
            "ok \xEF\xBF\xBD x \xEF\xBF\xBD"
            "a \xEF\xBF\xBD\xEF\xBF\xBD "
            "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST(SnippetTest, MultiByteCountedAsCodePoints) {
  // 100 x "é" (2 bytes each) then a space and the term, then 300 x "€".
  std::string text;
  for (int i = 0; i < 100; ++i) text += "\xC3\xA9";
  text += " needle ";
  for (int i = 0; i < 300; ++i) text += "\xE2\x82\xAC";
  const std::string s = makeSnippet(text, {"needle"});
  // p = 101 -> s = 41: 59 x "é", " needle " (8), 133 x "€".
  std::string expected = "...";
  for (int i = 0; i < 59; ++i) expected += "\xC3\xA9";
  expected += " needle ";
  for (int i = 0; i < 133; ++i) expected += "\xE2\x82\xAC";
  expected += "...";
  EXPECT_EQ(s, expected);
  EXPECT_EQ(codePoints(s), 206u);
}

TEST(SnippetTest, AtMost206CodePointsAndNoControlWhitespace) {
  std::string text;
  for (int i = 0; i < 2000; ++i) text += (i % 7 == 0) ? "\n\t" : "w\xC3\xB6rd ";
  text += " needle ";
  text += text;
  const std::string s = makeSnippet(text, {"needle"});
  EXPECT_LE(codePoints(s), 206u);
  EXPECT_EQ(s.find_first_of("\n\t\r\f\v"), std::string::npos);
}

TEST(SnippetTest, NoMatchStartsAtZeroAndEmptyText) {
  EXPECT_EQ(makeSnippet("abc def", {"zzz"}), "abc def");
  EXPECT_EQ(makeSnippet("", {"zzz"}), "");
  EXPECT_EQ(makeSnippet(" \n\t ", {"zzz"}), "");
}

TEST(SnippetTest, FromFileAndMissingFile) {
  const auto path =
      std::filesystem::temp_directory_path() / "snippet_test_file.txt";
  { std::ofstream(path, std::ios::binary) << "Line one\r\nline Two"; }
  EXPECT_EQ(search::makeSnippetFromFile(path, {"two"}), "Line one line Two");
  std::filesystem::remove(path);
  EXPECT_EQ(search::makeSnippetFromFile(path, {"two"}), "");
}

// ---------------------------------------------------------------------------
// Equivalence with the previous straightforward implementation.
// ---------------------------------------------------------------------------

namespace {

namespace reference {

// Verbatim copy of the pre-streaming implementation (whole-buffer decode,
// whole-text normalisation, linear token scan). Kept here only as an oracle.

constexpr char32_t kReplacement = 0xFFFD;

bool inRange(unsigned char b, unsigned char lo, unsigned char hi) {
  return b >= lo && b <= hi;
}

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
    unsigned char lo = 0x80, hi = 0xBF;
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
    i = j;
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
  return c < 0x80 && search::isTokenChar(static_cast<unsigned char>(c));
}

std::string makeSnippet(std::string_view fileBytes,
                        const std::vector<std::string>& expandedTerms) {
  if (fileBytes.substr(0, 3) == "\xEF\xBB\xBF") fileBytes.remove_prefix(3);
  const std::u32string decoded = decodeUtf8(fileBytes);

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
    if (tokenLength < search::kMinTokenLength ||
        tokenLength > search::kMaxTokenLength) {
      continue;
    }
    token.clear();
    for (std::size_t k = start; k < i; ++k) {
      token.push_back(search::toLowerAscii(static_cast<char>(text[k])));
    }
    if (search::isStopWord(token)) continue;
    if (wanted.count(token) != 0) {
      p = start;
      found = true;
    }
  }

  const std::size_t s = p > 60 ? p - 60 : 0;
  const std::size_t e = std::min(len, s + 200);
  std::string snippet;
  if (s > 0) snippet += "...";
  for (std::size_t k = s; k < e; ++k) appendUtf8(snippet, text[k]);
  if (e < len) snippet += "...";
  return snippet;
}

}  // namespace reference

// Random document generator covering the §4.6 edge cases.
class FuzzInput {
 public:
  explicit FuzzInput(std::uint32_t seed) : rng_(seed) {}

  int uniform(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng_);
  }

  // A random piece of text; `target` is never produced here.
  std::string piece() {
    static const char* const kWords[] = {
        "alpha", "Beta",  "gamma", "THE",   "and",  "of",  "x",  "a1",
        "zz",    "lorem", "Ipsum", "dolor", "42",   "is",  "it", "word",
        "ab",    "x9y8",  "end",   "start", "data", "Max", "on", "to"};
    switch (uniform(0, 19)) {
      case 0:
      case 1:
      case 2:
      case 3:
      case 4:
      case 5:
        return kWords[uniform(0, std::size(kWords) - 1)];
      case 6:
      case 7:
        return std::string(uniform(1, 3), ' ');
      case 8: {  // whitespace run, sometimes long
        static const char kSpaces[] = " \t\n\r\f\v";
        std::string out(uniform(0, 3) == 0 ? uniform(50, 400) : uniform(1, 6),
                        ' ');
        for (char& c : out) c = kSpaces[uniform(0, 5)];
        return out;
      }
      case 9: {  // punctuation
        static const char kPunct[] = ".,;:!?-_()[]'\"/";
        return std::string(1, kPunct[uniform(0, std::size(kPunct) - 2)]);
      }
      case 10: {  // valid multi-byte characters
        static const char* const kMulti[] = {
            "\xC3\xA9",     "\xD0\xBF",         "\xE2\x82\xAC",
            "\xE4\xB8\xAD", "\xF0\x9F\x98\x80", "\xEF\xBB\xBF"};  // U+FEFF
        std::string out;
        for (int i = uniform(1, 4); i > 0; --i) {
          out += kMulti[uniform(0, std::size(kMulti) - 1)];
        }
        return out;
      }
      case 11: {  // invalid / truncated sequences
        static const char* const kBad[] = {
            "\xFF",     "\x80",         "\xBF\xBF",     "\xC0\x80",
            "\xC1",     "\xE2\x82",     "\xE0\x80\x80", "\xED\xA0\x80",
            "\xF0\x80", "\xF4\x90\x80", "\xF5",         "\xF0\x9F\x98",
            "\xEF\xBB", "\xC3",         "\xE2",         "\xF0"};
        return kBad[uniform(0, std::size(kBad) - 1)];
      }
      case 12: {  // token runs around the 64-character limit
        static const int kLengths[] = {63, 64, 65, 100, 300};
        return std::string(kLengths[uniform(0, 4)], 'k');
      }
      case 13:
        return std::string(uniform(1, 30), 'q');
      default: {  // random printable ASCII
        std::string out(uniform(1, 8), ' ');
        for (char& c : out) c = static_cast<char>(uniform(0x21, 0x7E));
        return out;
      }
    }
  }

  // Concatenation of random pieces, about `approxBytes` long.
  std::string text(std::size_t approxBytes) {
    std::string out;
    while (out.size() < approxBytes) out += piece();
    return out;
  }

 private:
  std::mt19937 rng_;
};

struct FuzzCase {
  std::string bytes;
  std::vector<std::string> terms;
};

FuzzCase makeFuzzCase(FuzzInput& gen) {
  static const std::size_t kSizes[] = {0, 5, 40, 150, 210, 300, 1000, 5000};
  const std::size_t size = kSizes[gen.uniform(0, std::size(kSizes) - 1)];
  FuzzCase c;
  // Expanded terms: some filler, the target, sometimes a common word.
  for (int i = gen.uniform(0, 47); i > 0; --i) {
    c.terms.push_back("filler" + std::to_string(i));
  }
  const int placement = gen.uniform(0, 4);  // start, middle, end, absent, word
  const std::string target = gen.uniform(0, 1) ? "target" : "needle";
  if (placement != 3) c.terms.push_back(target);
  if (placement == 4 || gen.uniform(0, 3) == 0) c.terms.push_back("alpha");
  if (gen.uniform(0, 5) == 0) c.terms.push_back("the");  // stop word
  if (gen.uniform(0, 5) == 0) c.terms.push_back(std::string(64, 'k'));
  if (gen.uniform(0, 5) == 0) c.terms.push_back(std::string(65, 'k'));

  const std::string body = gen.text(size);
  const std::string shown = gen.uniform(0, 1) ? target : "TaRgEt";
  std::string doc;
  switch (placement) {
    case 0:
      doc = shown + " " + body;
      break;
    case 1: {
      const std::size_t cut = body.size() / 2;
      doc = body.substr(0, cut) + " " + shown + " " + body.substr(cut);
      break;
    }
    case 2:
      doc = body + " " + shown + gen.text(gen.uniform(0, 1) ? 0 : 3);
      break;
    default:
      doc = body;
      break;
  }
  if (gen.uniform(0, 3) == 0) doc = "\xEF\xBB\xBF" + doc;  // BOM
  if (gen.uniform(0, 5) == 0) doc = std::string(gen.uniform(1, 90), ' ') + doc;
  if (gen.uniform(0, 5) == 0) doc += std::string(gen.uniform(1, 90), '\n');
  c.bytes = std::move(doc);
  return c;
}

}  // namespace

TEST(SnippetTest, StreamingMatchesReferenceOnGeneratedInputs) {
  FuzzInput gen(12345);
  static const std::size_t kChunks[] = {1, 2, 3, 7, 13, 4096};
  for (int i = 0; i < 1500; ++i) {
    const FuzzCase c = makeFuzzCase(gen);
    const std::string expected = reference::makeSnippet(c.bytes, c.terms);
    ASSERT_EQ(makeSnippet(c.bytes, c.terms), expected) << "case " << i;
    for (std::size_t chunk : kChunks) {
      ASSERT_EQ(search::detail::makeSnippetChunked(c.bytes, c.terms, chunk),
                expected)
          << "case " << i << " chunk " << chunk;
    }
  }
}

TEST(SnippetTest, StreamingMatchesReferenceOnBoundaryLengths) {
  // Texts of every length around the window size, with and without a
  // match, a trailing whitespace run, and a match exactly at p = 60 / 61.
  for (std::size_t len = 0; len <= 270; ++len) {
    std::string text(len, 'w');
    for (std::size_t k = 7; k < len; k += 9) text[k] = ' ';
    for (const std::string& doc :
         {text, text + "   \n", "   " + text, text + " needle",
          std::string(59, 'x') + " needle " + text,
          std::string(60, 'x') + " needle " + text}) {
      for (const std::vector<std::string>& terms :
           {std::vector<std::string>{"needle"},
            std::vector<std::string>{"zzz"}}) {
        const std::string expected = reference::makeSnippet(doc, terms);
        ASSERT_EQ(makeSnippet(doc, terms), expected) << "len " << len;
        for (std::size_t chunk : {1, 2, 3, 7}) {
          ASSERT_EQ(search::detail::makeSnippetChunked(doc, terms, chunk),
                    expected)
              << "len " << len << " chunk " << chunk;
        }
      }
    }
  }
}

TEST(SnippetTest, FileVariantMatchesReferenceWithTinyChunks) {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("snippet_equiv_" +
       std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
       ".txt");
  FuzzInput gen(777);
  static const std::size_t kChunks[] = {
      1, 2, 3, 7, 64, search::detail::kSnippetReadChunk};
  for (int i = 0; i < 150; ++i) {
    const FuzzCase c = makeFuzzCase(gen);
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out.write(c.bytes.data(), static_cast<std::streamsize>(c.bytes.size()));
    }
    const std::string expected = reference::makeSnippet(c.bytes, c.terms);
    ASSERT_EQ(search::makeSnippetFromFile(path, c.terms), expected)
        << "case " << i;
    for (std::size_t chunk : kChunks) {
      ASSERT_EQ(
          search::detail::makeSnippetFromFileChunked(path, c.terms, chunk),
          expected)
          << "case " << i << " chunk " << chunk;
    }
  }
  std::filesystem::remove(path);
}

TEST(SnippetTest, StopsReadingAfterEarlyMatch) {
  // A match at the start followed by an 8 MiB tail: only the first ~200
  // code points determine the result.
  const std::string doc = "needle " + std::string(8 << 20, 'a');
  const std::string s = makeSnippet(doc, {"needle"});
  EXPECT_EQ(s, doc.substr(0, 200) + "...");
}
