#include <gtest/gtest.h>

#include "core/TextProcessor.hpp"

using search::core::TextProcessor;

TEST(TextProcessorTest, NormalizeLowercasesAndRemovesPunctuation) {
    EXPECT_EQ(TextProcessor::normalize("Hello, World! C++20"), "hello  world  c  20");
}

TEST(TextProcessorTest, NormalizationKeepsLength) {
    const std::string text = "Machine-Learning (ML) & AI: 2024.";
    EXPECT_EQ(TextProcessor::normalize(text).size(), text.size());
}

TEST(TextProcessorTest, TokenizeSplitsAndFiltersStopWords) {
    const auto tokens = TextProcessor::tokenize("The quick brown fox jumps over the lazy dog");
    const std::vector<std::string> expected = {"quick", "brown", "fox", "jumps", "lazy", "dog"};
    EXPECT_EQ(tokens, expected);
}

TEST(TextProcessorTest, TokenizeDropsSingleCharactersAndEmptyTokens) {
    const auto tokens = TextProcessor::tokenize("  a  b ,,, x  ok  !!  ");
    const std::vector<std::string> expected = {"ok"};
    EXPECT_EQ(tokens, expected);
}

TEST(TextProcessorTest, TokenizeEmptyInput) {
    EXPECT_TRUE(TextProcessor::tokenize("").empty());
    EXPECT_TRUE(TextProcessor::tokenize("   ...   ").empty());
    EXPECT_TRUE(TextProcessor::tokenize("the and of").empty());
}

TEST(TextProcessorTest, TokenizeHandlesNonAsciiBytesAsSeparators) {
    const auto tokens = TextProcessor::tokenize("caf\xC3\xA9 r\xC3\xA9sum\xC3\xA9 data");
    const std::vector<std::string> expected = {"caf", "sum", "data"};
    EXPECT_EQ(tokens, expected);
}

TEST(TextProcessorTest, TokenizeDropsOverlongTokens) {
    const std::string longToken(TextProcessor::kMaxTokenLength + 1, 'x');
    const auto tokens = TextProcessor::tokenize(longToken + " valid");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "valid");
}

TEST(TextProcessorTest, UniqueTokensKeepsFirstOccurrenceOrder) {
    const auto tokens = TextProcessor::uniqueTokens("Data science, data MINING and Data");
    const std::vector<std::string> expected = {"data", "science", "mining"};
    EXPECT_EQ(tokens, expected);
}

TEST(TextProcessorTest, StopWordDetection) {
    EXPECT_TRUE(TextProcessor::isStopWord("the"));
    EXPECT_TRUE(TextProcessor::isStopWord("between"));
    EXPECT_FALSE(TextProcessor::isStopWord("engine"));
    EXPECT_FALSE(TextProcessor::isIndexable("x"));
    EXPECT_TRUE(TextProcessor::isIndexable("ai"));
}

TEST(TextProcessorTest, ForEachTokenProducesViewsIntoInput) {
    std::string text = "Parallel COMPUTING with threads";
    TextProcessor::normalizeInPlace(text);
    std::vector<std::string_view> views;
    TextProcessor::forEachToken(text, [&views](std::string_view t) { views.push_back(t); });
    ASSERT_EQ(views.size(), 3u);
    EXPECT_EQ(views[0], "parallel");
    EXPECT_EQ(views[1], "computing");
    EXPECT_EQ(views[2], "threads");
    EXPECT_GE(views[0].data(), text.data());
    EXPECT_LE(views[2].data() + views[2].size(), text.data() + text.size());
}
