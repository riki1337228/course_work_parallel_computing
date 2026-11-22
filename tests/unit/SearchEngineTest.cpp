#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include "TestUtils.hpp"
#include "search/SearchEngine.hpp"

using search::engine::IndexNotReadyError;
using search::engine::QueryTerm;
using search::engine::SearchEngine;
using search::index::InvertedIndex;
using search::test::TempDir;

namespace {

std::shared_ptr<InvertedIndex> makeIndex() {
    auto index = std::make_shared<InvertedIndex>();
    index->addDocument("doc_ml", "", "machine learning machine learning neural network training");  // 0
    index->addDocument("doc_car", "", "car engine vehicle automobile road");                           // 1
    index->addDocument("doc_mixed", "", "machine shop repairs car engine");                            // 2
    index->addDocument("doc_bio", "", "cell biology protein genome");                                  // 3
    index->finalize();
    return index;
}

std::vector<QueryTerm> terms(std::initializer_list<const char*> list) {
    std::vector<QueryTerm> out;
    for (const char* t : list) out.push_back({t, 1.0, false});
    return out;
}

}  // namespace

TEST(SearchEngineTest, IdfFormula) {
    EXPECT_DOUBLE_EQ(SearchEngine::inverseDocumentFrequency(10, 0), 0.0);
    EXPECT_DOUBLE_EQ(SearchEngine::inverseDocumentFrequency(10, 10), 1.0);
    EXPECT_DOUBLE_EQ(SearchEngine::inverseDocumentFrequency(100, 10), std::log(10.0) + 1.0);
}

TEST(SearchEngineTest, SingleTermTfIdfScore) {
    const auto index = makeIndex();
    const auto response = SearchEngine::rank(*index, terms({"biology"}), 10, false);
    ASSERT_EQ(response.results.size(), 1u);
    EXPECT_EQ(response.results[0].docId, "doc_bio");
    // tf = 1/4, idf = ln(4/1) + 1
    EXPECT_NEAR(response.results[0].score, 0.25 * (std::log(4.0) + 1.0), 1e-12);
    EXPECT_EQ(response.totalMatches, 1u);
}

TEST(SearchEngineTest, ResultsAreSortedByScoreDescending) {
    const auto index = makeIndex();
    const auto response = SearchEngine::rank(*index, terms({"machine", "car"}), 10, false);
    ASSERT_EQ(response.results.size(), 3u);
    for (std::size_t i = 1; i < response.results.size(); ++i) {
        EXPECT_GE(response.results[i - 1].score, response.results[i].score);
    }
    EXPECT_EQ(response.totalMatches, 3u);
}

TEST(SearchEngineTest, DocumentMatchingMoreTermsRanksHigher) {
    const auto index = makeIndex();
    const auto response = SearchEngine::rank(*index, terms({"car", "engine", "shop"}), 10, false);
    ASSERT_FALSE(response.results.empty());
    EXPECT_EQ(response.results[0].docId, "doc_mixed");
}

TEST(SearchEngineTest, LimitTruncatesResults) {
    const auto index = makeIndex();
    const auto response = SearchEngine::rank(*index, terms({"machine", "car", "cell"}), 2, false);
    EXPECT_EQ(response.results.size(), 2u);
    EXPECT_EQ(response.totalMatches, 4u);
}

TEST(SearchEngineTest, UnknownTermsGiveNoResults) {
    const auto index = makeIndex();
    const auto response = SearchEngine::rank(*index, terms({"quasar"}), 10, false);
    EXPECT_TRUE(response.results.empty());
    EXPECT_EQ(response.totalMatches, 0u);
    EXPECT_TRUE(SearchEngine::rank(*index, {}, 10, false).results.empty());
}

TEST(SearchEngineTest, ExpandedTermsHaveLowerWeight) {
    const auto index = makeIndex();
    std::vector<QueryTerm> q = {{"vehicle", 0.5, true}};
    const auto half = SearchEngine::rank(*index, q, 10, false);
    q[0].weight = 1.0;
    const auto full = SearchEngine::rank(*index, q, 10, false);
    ASSERT_EQ(half.results.size(), 1u);
    EXPECT_NEAR(half.results[0].score * 2.0, full.results[0].score, 1e-12);
}

TEST(SearchEngineTest, SearchBeforeIndexThrows) {
    SearchEngine engine;
    EXPECT_FALSE(engine.ready());
    EXPECT_THROW((void)engine.search(terms({"car"}), 10), IndexNotReadyError);
}

TEST(SearchEngineTest, SearchUsesPublishedIndexAndSnippets) {
    TempDir dir;
    const auto path = dir.write("doc.txt", "Electric car batteries are improving quickly.");
    auto index = std::make_shared<InvertedIndex>();
    index->addDocument("document_000001", path, "Electric car batteries are improving quickly.");
    index->finalize();
    SearchEngine engine;
    engine.setIndex(index);
    EXPECT_TRUE(engine.ready());
    const auto response = engine.search(terms({"batteries"}), 5);
    ASSERT_EQ(response.results.size(), 1u);
    EXPECT_EQ(response.results[0].docId, "document_000001");
    EXPECT_NE(response.results[0].snippet.find("batteries"), std::string::npos);
    EXPECT_GE(response.timeMs, 0.0);
}

TEST(SearchEngineTest, BuildQueryTermsDeduplicatesAndWeights) {
    const auto q = SearchEngine::buildQueryTerms("The Car and car engine", {"automobile", "car", "motor vehicle"}, 0.5);
    ASSERT_EQ(q.size(), 5u);
    EXPECT_EQ(q[0].term, "car");
    EXPECT_FALSE(q[0].expanded);
    EXPECT_DOUBLE_EQ(q[0].weight, 1.0);
    EXPECT_EQ(q[1].term, "engine");
    EXPECT_EQ(q[2].term, "automobile");
    EXPECT_TRUE(q[2].expanded);
    EXPECT_DOUBLE_EQ(q[2].weight, 0.5);
    EXPECT_EQ(q[3].term, "motor");
    EXPECT_EQ(q[4].term, "vehicle");
}

TEST(SearchEngineTest, BuildQueryTermsOnlyStopWords) {
    EXPECT_TRUE(SearchEngine::buildQueryTerms("the and of", {}, 0.5).empty());
}
