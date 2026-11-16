#include "search/tokenizer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using search::buildQueryTerms;
using search::tokenize;
using V = std::vector<std::string>;

TEST(TokenizerTest, ApiExamples) {
  EXPECT_EQ(tokenize("Hello, World!"), (V{"hello", "world"}));
  EXPECT_EQ(tokenize("C++20 and e-mail"), (V{"20", "mail"}));
  EXPECT_EQ(tokenize("don't"), (V{"don"}));
}

TEST(TokenizerTest, AllStopWordsRemoved) {
  const std::string all =
      "an and are as at be but by for from has have he her his in is it its "
      "of on or she that the their they this to was were will with";
  EXPECT_TRUE(tokenize(all).empty());
  EXPECT_TRUE(tokenize("THE And WiTh").empty());
  EXPECT_TRUE(search::isStopWord("the"));
  EXPECT_FALSE(search::isStopWord("then"));
  EXPECT_FALSE(search::isStopWord("not"));  // not in the list
}

TEST(TokenizerTest, LengthLimits) {
  const std::string len64(64, 'x');
  const std::string len65(65, 'y');
  EXPECT_EQ(tokenize("a " + len64 + " " + len65 + " ok"), (V{len64, "ok"}));
}

TEST(TokenizerTest, NonAsciiBytesAreSeparators) {
  // "café" (é = C3 A9), "naïve" (ï = C3 AF), em dash, invalid byte 0xFF.
  EXPECT_EQ(tokenize("caf\xC3\xA9 na\xC3\xAFve x\xE2\x80\x94yz ab\xFF"
                     "cd"),
            (V{"caf", "na", "ve", "yz", "ab", "cd"}));
}

TEST(TokenizerTest, UnderscoreDigitsAndCase) {
  EXPECT_EQ(tokenize("foo_bar MiXeD 2024 x1"),
            (V{"foo", "bar", "mixed", "2024", "x1"}));
}

TEST(TokenizerTest, ByteOffsetsReported) {
  std::vector<std::size_t> offsets;
  search::forEachToken(
      "  Hi, the World",
      [&](std::string_view, std::size_t at) { offsets.push_back(at); });
  EXPECT_EQ(offsets, (std::vector<std::size_t>{2, 10}));
}

TEST(QueryTermsTest, OriginalThenExpansionWithWeights) {
  auto terms = buildQueryTerms("Climate change, climate!",
                               {"global warming", "Climate", "weather-change"});
  ASSERT_EQ(terms.size(), 5u);
  EXPECT_EQ(terms[0].term, "climate");
  EXPECT_EQ(terms[1].term, "change");
  EXPECT_DOUBLE_EQ(terms[1].weight, 1.0);
  EXPECT_EQ(terms[2].term, "global");
  EXPECT_EQ(terms[3].term, "warming");
  EXPECT_EQ(terms[4].term, "weather");
  EXPECT_DOUBLE_EQ(terms[4].weight, 0.5);
}

TEST(QueryTermsTest, Truncation) {
  std::string query;
  for (int i = 0; i < 40; ++i) query += "q" + std::to_string(i) + " ";
  std::vector<std::string> expansion;
  for (int i = 0; i < 20; ++i) expansion.push_back("e" + std::to_string(i));
  auto terms = buildQueryTerms(query, expansion);
  ASSERT_EQ(terms.size(), 32u + 16u);
  EXPECT_EQ(terms[31].term, "q31");
  EXPECT_EQ(terms[32].term, "e0");
  EXPECT_EQ(terms[47].term, "e15");
}

TEST(QueryTermsTest, StopWordOnlyQueryIsEmpty) {
  EXPECT_TRUE(buildQueryTerms("the of !!", {}).empty());
  EXPECT_TRUE(search::originalQueryTokens("the of !!").empty());
}
