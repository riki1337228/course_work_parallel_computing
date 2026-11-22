#include <gtest/gtest.h>

#include <memory>

#include "TestUtils.hpp"
#include "search/SearchService.hpp"

using search::engine::IndexBuildResult;
using search::engine::IndexNotReadyError;
using search::engine::SearchService;
using search::ollama::ExpansionStatus;
using search::test::FakeLlmClient;
using search::test::TempDir;

namespace {

search::core::Config makeConfig(const std::filesystem::path& dataset) {
    search::core::Config cfg;
    cfg.datasetPath = dataset.string();
    cfg.requestThreads = 2;
    cfg.maxExpansionTerms = 4;
    return cfg;
}

void writeDocs(const TempDir& dir) {
    dir.write("documents/document_000001.txt", "The automobile industry builds every vehicle with an engine.");
    dir.write("documents/document_000002.txt", "A car needs fuel. The car engine burns fuel.");
    dir.write("documents/document_000003.txt", "Protein folding in cell biology.");
}

}  // namespace

TEST(SearchServiceTest, SearchBeforeIndexingThrows) {
    TempDir dir;
    writeDocs(dir);
    SearchService service(makeConfig(dir.path()), std::make_shared<FakeLlmClient>());
    EXPECT_FALSE(service.indexReady());
    EXPECT_THROW(service.runPipeline("car", 10, false), IndexNotReadyError);
    const auto stats = service.stats();
    EXPECT_EQ(stats.documents, 0u);
    EXPECT_FALSE(stats.indexReady);
}

TEST(SearchServiceTest, BuildIndexAndSearchWithExpansion) {
    TempDir dir;
    writeDocs(dir);
    auto llm = std::make_shared<FakeLlmClient>();
    llm->setResponse(R"({"original":"car","expanded_terms":["automobile","vehicle"]})");
    SearchService service(makeConfig(dir.path()), llm);

    const auto build = service.buildIndex(2);
    ASSERT_EQ(build.error, IndexBuildResult::Error::None) << build.message;
    EXPECT_EQ(build.documentsIndexed, 3u);

    const auto result = service.runPipeline("car", 10, true);
    EXPECT_EQ(result.expansion.status, ExpansionStatus::Ok);
    ASSERT_EQ(result.search.results.size(), 2u);  // doc 2 (car) and doc 1 (automobile, vehicle)
    EXPECT_EQ(result.search.results[0].docId, "document_000002");
    EXPECT_EQ(result.search.results[1].docId, "document_000001");

    const auto stats = service.stats();
    EXPECT_EQ(stats.documents, 3u);
    EXPECT_EQ(stats.threadsUsed, 2u);
    EXPECT_TRUE(stats.indexReady);
    EXPECT_EQ(stats.searchesServed, 1u);
}

TEST(SearchServiceTest, FallbackStillReturnsKeywordResults) {
    TempDir dir;
    writeDocs(dir);
    auto llm = std::make_shared<FakeLlmClient>();
    llm->available = false;
    SearchService service(makeConfig(dir.path()), llm);
    ASSERT_EQ(service.buildIndex(1).error, IndexBuildResult::Error::None);
    const auto result = service.runPipeline("car", 10, true);
    EXPECT_EQ(result.expansion.status, ExpansionStatus::Fallback);
    ASSERT_EQ(result.search.results.size(), 1u);
    EXPECT_EQ(result.search.results[0].docId, "document_000002");
    EXPECT_FALSE(service.ollamaAvailable());
}

TEST(SearchServiceTest, ExpansionDisabledByRequest) {
    TempDir dir;
    writeDocs(dir);
    auto llm = std::make_shared<FakeLlmClient>();
    SearchService service(makeConfig(dir.path()), llm);
    ASSERT_EQ(service.buildIndex(1).error, IndexBuildResult::Error::None);
    const auto result = service.runPipeline("car", 10, false);
    EXPECT_EQ(result.expansion.status, ExpansionStatus::Disabled);
    EXPECT_EQ(llm->calls.load(), 0);
}

TEST(SearchServiceTest, EmptyAndMissingDatasetErrors) {
    TempDir empty;
    std::filesystem::create_directories(empty.path() / "documents");
    SearchService a(makeConfig(empty.path()), nullptr);
    EXPECT_EQ(a.buildIndex(2).error, IndexBuildResult::Error::DatasetEmpty);
    EXPECT_FALSE(a.indexReady());

    SearchService b(makeConfig("/definitely/not/existing"), nullptr);
    EXPECT_EQ(b.buildIndex(2).error, IndexBuildResult::Error::DatasetNotFound);
}

TEST(SearchServiceTest, RebuildReplacesIndexAndUpdatesStats) {
    TempDir dir;
    writeDocs(dir);
    SearchService service(makeConfig(dir.path()), nullptr);
    ASSERT_EQ(service.buildIndex(1).error, IndexBuildResult::Error::None);
    dir.write("documents/document_000004.txt", "Another car article.");
    const auto second = service.buildIndex(4);
    ASSERT_EQ(second.error, IndexBuildResult::Error::None);
    EXPECT_EQ(second.documentsIndexed, 4u);
    EXPECT_EQ(service.stats().threadsUsed, 4u);
    EXPECT_EQ(service.stats().documents, 4u);
}

TEST(SearchServiceTest, AsyncBuildCompletes) {
    TempDir dir;
    writeDocs(dir);
    SearchService service(makeConfig(dir.path()), nullptr);
    service.buildIndexAsync(2);
    service.shutdown();  // joins the background build
    EXPECT_TRUE(service.indexReady());
}
