#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <thread>

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

TEST(SearchServiceTest, RoutesSearchesByExpansion) {
    TempDir dir;
    writeDocs(dir);
    auto cfg = makeConfig(dir.path());
    cfg.llmThreads = 1;
    cfg.llmMaxQueue = 2;
    SearchService service(cfg, std::make_shared<FakeLlmClient>());
    EXPECT_EQ(service.route(false), SearchService::Route::Plain);
    EXPECT_EQ(service.route(true), SearchService::Route::Llm);
    EXPECT_EQ(&service.poolFor(SearchService::Route::Llm), &service.llmPool());
    EXPECT_EQ(&service.poolFor(SearchService::Route::Plain), &service.requestPool());
    EXPECT_EQ(&service.poolFor(SearchService::Route::Overloaded), &service.requestPool());

    // Occupy the single LLM worker and fill its queue -> new LLM searches are overloaded.
    std::promise<void> release;
    auto gate = release.get_future().share();
    for (int i = 0; i < 3; ++i) {
        service.llmPool().post([gate] { gate.wait(); });
    }
    for (int i = 0; i < 100 && service.llmPool().pendingTasks() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(service.route(true), SearchService::Route::Overloaded);
    EXPECT_EQ(service.route(false), SearchService::Route::Plain);  // plain searches unaffected
    release.set_value();
    service.llmPool().waitAll();
    EXPECT_EQ(service.route(true), SearchService::Route::Llm);
}

TEST(SearchServiceTest, OverloadedPipelineFallsBackImmediately) {
    TempDir dir;
    writeDocs(dir);
    auto llm = std::make_shared<FakeLlmClient>();
    SearchService service(makeConfig(dir.path()), llm);
    ASSERT_EQ(service.buildIndex(1).error, IndexBuildResult::Error::None);
    const auto result = service.runPipeline("car", 10, true, SearchService::Clock::now(), /*overloaded=*/true);
    EXPECT_EQ(result.expansion.status, ExpansionStatus::Fallback);
    EXPECT_NE(result.expansion.fallbackReason.find("overloaded"), std::string::npos);
    EXPECT_EQ(llm->calls.load(), 0);
    EXPECT_EQ(result.search.results.size(), 1u);
}

TEST(SearchServiceTest, BudgetIsCountedFromArrival) {
    TempDir dir;
    writeDocs(dir);
    auto llm = std::make_shared<FakeLlmClient>();
    auto cfg = makeConfig(dir.path());
    cfg.ollamaTimeoutSec = 5.0;
    SearchService service(cfg, llm);
    ASSERT_EQ(service.buildIndex(1).error, IndexBuildResult::Error::None);
    // The request "arrived" 6 s ago: its 5 s budget is already spent in the queue.
    const auto old = SearchService::Clock::now() - std::chrono::seconds(6);
    const auto result = service.runPipeline("car", 10, true, old);
    EXPECT_EQ(result.expansion.status, ExpansionStatus::Fallback);
    EXPECT_EQ(llm->calls.load(), 0);
    EXPECT_GE(result.totalTimeMs, 6000.0);  // total time includes the waiting time
}

TEST(SearchServiceTest, AsyncBuildCompletes) {
    TempDir dir;
    writeDocs(dir);
    SearchService service(makeConfig(dir.path()), nullptr);
    service.buildIndexAsync(2);
    service.shutdown();  // joins the background build
    EXPECT_TRUE(service.indexReady());
}
