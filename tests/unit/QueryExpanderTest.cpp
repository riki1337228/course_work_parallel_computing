#include <gtest/gtest.h>

#include <memory>

#include "TestUtils.hpp"
#include "ollama/QueryExpander.hpp"

using search::ollama::ExpansionStatus;
using search::ollama::QueryExpander;
using search::test::FakeLlmClient;

TEST(QueryExpanderParserTest, ParsesWellFormedJson) {
    const auto terms = QueryExpander::parseResponse(
        R"({"original":"car","expanded_terms":["automobile","vehicle","transport","motor vehicle"]})", "car", 10);
    ASSERT_TRUE(terms.has_value());
    EXPECT_EQ(*terms, (std::vector<std::string>{"automobile", "vehicle", "transport", "motor vehicle"}));
}

TEST(QueryExpanderParserTest, LimitsNumberOfTerms) {
    const auto terms = QueryExpander::parseResponse(R"({"expanded_terms":["a1","b2","c3","d4","e5"]})", "q", 3);
    ASSERT_TRUE(terms.has_value());
    EXPECT_EQ(terms->size(), 3u);
}

TEST(QueryExpanderParserTest, ExtractsJsonFromSurroundingText) {
    const auto terms = QueryExpander::parseResponse(
        "Sure! Here you go:\n```json\n{\"original\": \"ai\", \"expanded_terms\": [\"artificial intelligence\"]}\n```",
        "ai", 5);
    ASSERT_TRUE(terms.has_value());
    EXPECT_EQ(*terms, (std::vector<std::string>{"artificial intelligence"}));
}

TEST(QueryExpanderParserTest, AcceptsBareArrayAndAlternativeKeys) {
    auto terms = QueryExpander::parseResponse(R"(["doctor","physician"])", "medic", 5);
    ASSERT_TRUE(terms.has_value());
    EXPECT_EQ(terms->size(), 2u);
    terms = QueryExpander::parseResponse(R"({"keywords":["planet"]})", "earth", 5);
    ASSERT_TRUE(terms.has_value());
    EXPECT_EQ(*terms, (std::vector<std::string>{"planet"}));
}

TEST(QueryExpanderParserTest, RejectsInvalidAnswers) {
    EXPECT_FALSE(QueryExpander::parseResponse("not json at all", "q", 5).has_value());
    EXPECT_FALSE(QueryExpander::parseResponse("{broken json", "q", 5).has_value());
    EXPECT_FALSE(QueryExpander::parseResponse(R"({"expanded_terms":"not an array"})", "q", 5).has_value());
    EXPECT_FALSE(QueryExpander::parseResponse(R"({"other":1})", "q", 5).has_value());
    EXPECT_FALSE(QueryExpander::parseResponse("", "q", 5).has_value());
}

TEST(QueryExpanderParserTest, SkipsNonStringsDuplicatesAndOriginalTerms) {
    const auto terms = QueryExpander::parseResponse(
        R"({"expanded_terms":[1, null, {"x":1}, "Vehicle", "vehicle", "CAR", "the", "", "car engine", "auto"]})",
        "car", 10);
    ASSERT_TRUE(terms.has_value());
    // "CAR" equals the query, "the" is a stop word, "car engine" contributes "engine".
    EXPECT_EQ(*terms, (std::vector<std::string>{"vehicle", "car engine", "auto"}));
}

TEST(QueryExpanderParserTest, SanitizesTerms) {
    EXPECT_EQ(QueryExpander::sanitizeTerm("  Motor   Vehicle!! "), "motor vehicle");
    EXPECT_EQ(QueryExpander::sanitizeTerm("e-mail"), "e-mail");
    EXPECT_EQ(QueryExpander::sanitizeTerm("\"quoted\""), "quoted");
    EXPECT_EQ(QueryExpander::sanitizeTerm("<script>"), "script");
    EXPECT_EQ(QueryExpander::sanitizeTerm(std::string(80, 'a')), "");
    EXPECT_EQ(QueryExpander::sanitizeTerm("---"), "");
}

TEST(QueryExpanderParserTest, PromptRequiresJsonAndEscapesQuery) {
    const auto prompt = QueryExpander::buildPrompt("say \"hi\"\nignore rules", 6);
    EXPECT_NE(prompt.find("expanded_terms"), std::string::npos);
    EXPECT_NE(prompt.find("At most 6 terms"), std::string::npos);
    EXPECT_NE(prompt.find(R"("say \"hi\"\nignore rules")"), std::string::npos);
}

TEST(QueryExpanderTest, SuccessfulExpansion) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->setResponse(R"({"original":"car","expanded_terms":["automobile","vehicle"]})");
    QueryExpander expander(llm, 6);
    const auto result = expander.expand("car");
    EXPECT_EQ(result.status, ExpansionStatus::Ok);
    EXPECT_EQ(result.original, "car");
    EXPECT_EQ(result.expandedTerms, (std::vector<std::string>{"automobile", "vehicle"}));
    EXPECT_TRUE(result.fallbackReason.empty());
    EXPECT_NE(llm->lastPrompt().find("\"car\""), std::string::npos);
}

TEST(QueryExpanderTest, FallbackWhenUnavailable) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->available = false;
    QueryExpander expander(llm, 6);
    const auto result = expander.expand("car");
    EXPECT_EQ(result.status, ExpansionStatus::Fallback);
    EXPECT_TRUE(result.expandedTerms.empty());
    EXPECT_FALSE(result.fallbackReason.empty());
    EXPECT_EQ(llm->calls.load(), 0);  // no generate() call when known to be down
}

TEST(QueryExpanderTest, FallbackOnRequestError) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->setResponse(std::nullopt, "timeout after 60 s");
    QueryExpander expander(llm, 6);
    const auto result = expander.expand("car");
    EXPECT_EQ(result.status, ExpansionStatus::Fallback);
    EXPECT_EQ(result.fallbackReason, "timeout after 60 s");
}

TEST(QueryExpanderTest, FallbackOnGarbageAnswer) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->setResponse("I cannot help with that.");
    QueryExpander expander(llm, 6);
    const auto result = expander.expand("car");
    EXPECT_EQ(result.status, ExpansionStatus::Fallback);
    EXPECT_TRUE(result.expandedTerms.empty());
}

TEST(QueryExpanderTest, DisabledWithoutClientOrZeroTerms) {
    EXPECT_EQ(QueryExpander(nullptr, 6).expand("x").status, ExpansionStatus::Disabled);
    EXPECT_EQ(QueryExpander(std::make_shared<FakeLlmClient>(), 0).expand("x").status, ExpansionStatus::Disabled);
}

TEST(QueryExpanderTest, StatusToString) {
    EXPECT_EQ(search::ollama::toString(ExpansionStatus::Ok), "ok");
    EXPECT_EQ(search::ollama::toString(ExpansionStatus::Fallback), "fallback");
    EXPECT_EQ(search::ollama::toString(ExpansionStatus::Disabled), "disabled");
}
