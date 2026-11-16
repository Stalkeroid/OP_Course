#include "search/snippet.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
