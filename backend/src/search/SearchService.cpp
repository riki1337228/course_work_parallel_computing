#include "search/SearchService.hpp"

#include <chrono>

namespace search::engine {

SearchService::SearchService(core::Config config, std::shared_ptr<ollama::LlmClient> llm)
    : config_(std::move(config)),
      llm_(std::move(llm)),
      expander_(llm_, config_.maxExpansionTerms),
      requestPool_(std::make_unique<threading::ThreadPool>(config_.requestThreads)) {}

SearchService::~SearchService() {
    shutdown();
}

IndexBuildResult SearchService::buildIndex(std::size_t threads) {
    IndexBuildResult result;
    result.threads = threads;

    bool expected = false;
    if (!indexing_.compare_exchange_strong(expected, true)) {
        result.error = IndexBuildResult::Error::InProgress;
        result.message = "index build is already in progress";
        return result;
    }
    // RAII reset of the flag, also on exceptions.
    struct FlagGuard {
        std::atomic<bool>& flag;
        ~FlagGuard() { flag.store(false); }
    } guard{indexing_};

    try {
        index::BuildStats stats;
        const index::IndexBuilder builder(threads);
        std::shared_ptr<index::InvertedIndex> built = builder.build(config_.datasetPath, &stats);
        engine_.setIndex(std::move(built));  // atomic publication of the new snapshot

        result.documentsIndexed = stats.documentsIndexed;
        result.timeMs = stats.totalTimeMs;
        result.stats = stats;
        std::lock_guard lock(statsMutex_);
        lastBuildTimeMs_ = stats.totalTimeMs;
        lastThreads_ = threads;
    } catch (const index::DatasetError& e) {
        result.error = e.kind() == index::DatasetError::Kind::Empty ? IndexBuildResult::Error::DatasetEmpty
                                                                    : IndexBuildResult::Error::DatasetNotFound;
        result.message = e.what();
    } catch (const std::exception& e) {
        result.error = IndexBuildResult::Error::Failed;
        result.message = std::string("index build failed: ") + e.what();
    }
    return result;
}

void SearchService::buildIndexAsync(std::size_t threads) {
    std::lock_guard lock(backgroundMutex_);
    if (backgroundBuild_.joinable()) {
        backgroundBuild_.join();
    }
    backgroundBuild_ = std::thread([this, threads] { buildIndex(threads); });
}

ollama::ExpansionResult SearchService::expandQuery(const std::string& query, bool enabled) const {
    if (!enabled) {
        ollama::ExpansionResult disabled;
        disabled.original = query;
        disabled.status = ollama::ExpansionStatus::Disabled;
        return disabled;
    }
    return expander_.expand(query);
}

SearchResponse SearchService::keywordSearch(const std::string& query, const std::vector<std::string>& expandedTerms,
                                            std::size_t limit) {
    const auto terms = SearchEngine::buildQueryTerms(query, expandedTerms, config_.expandedTermWeight);
    SearchResponse response = engine_.search(terms, limit);
    searchesServed_.fetch_add(1, std::memory_order_relaxed);
    return response;
}

PipelineResult SearchService::runPipeline(const std::string& query, std::size_t limit, bool expand) {
    const auto start = std::chrono::steady_clock::now();
    if (!engine_.ready()) {
        throw IndexNotReadyError();  // fail fast before spending time on the LLM
    }
    PipelineResult result;
    result.expansion = expandQuery(query, expand);
    result.search = keywordSearch(query, result.expansion.expandedTerms, limit);
    result.totalTimeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}

ServiceStats SearchService::stats() const {
    ServiceStats s;
    const auto snapshot = engine_.index();
    if (snapshot) {
        s.documents = snapshot->documentCount();
        s.uniqueTerms = snapshot->termCount();
        s.totalPostings = snapshot->postingCount();
        s.indexReady = true;
    }
    {
        std::lock_guard lock(statsMutex_);
        s.indexBuildTimeMs = lastBuildTimeMs_;
        s.threadsUsed = lastThreads_;
    }
    s.indexingInProgress = indexing_.load();
    s.searchesServed = searchesServed_.load();
    return s;
}

bool SearchService::ollamaAvailable() const {
    return llm_ && llm_->isAvailable();
}

std::string SearchService::ollamaModel() const {
    return llm_ ? llm_->model() : std::string{};
}

void SearchService::shutdown() {
    {
        std::lock_guard lock(backgroundMutex_);
        if (backgroundBuild_.joinable()) {
            backgroundBuild_.join();
        }
    }
    if (requestPool_) {
        requestPool_->shutdown();
    }
}

}  // namespace search::engine
