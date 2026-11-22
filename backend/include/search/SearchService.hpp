#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/Config.hpp"
#include "index/IndexBuilder.hpp"
#include "ollama/LlmClient.hpp"
#include "ollama/QueryExpander.hpp"
#include "search/SearchEngine.hpp"
#include "threading/ThreadPool.hpp"

namespace search::engine {

/// Outcome of POST /api/index.
struct IndexBuildResult {
    enum class Error { None, InProgress, DatasetNotFound, DatasetEmpty, Failed };
    Error error = Error::None;
    std::string message;
    std::size_t documentsIndexed = 0;
    std::size_t threads = 0;
    double timeMs = 0.0;
    index::BuildStats stats;
};

/// Snapshot of GET /api/stats.
struct ServiceStats {
    std::size_t documents = 0;
    double indexBuildTimeMs = 0.0;
    std::size_t threadsUsed = 0;
    std::size_t uniqueTerms = 0;
    std::size_t totalPostings = 0;
    bool indexReady = false;
    bool indexingInProgress = false;
    std::size_t searchesServed = 0;
};

/// Full result of the search pipeline (expansion + keyword search).
struct PipelineResult {
    ollama::ExpansionResult expansion;
    SearchResponse search;
    double totalTimeMs = 0.0;
};

/// Application service that owns the search engine, the query expander and the
/// worker pool used to execute requests off the HTTP event loop.
/// Framework-independent: the Drogon layer only adapts HTTP/WebSocket to it.
class SearchService {
public:
    SearchService(core::Config config, std::shared_ptr<ollama::LlmClient> llm);
    ~SearchService();

    SearchService(const SearchService&) = delete;
    SearchService& operator=(const SearchService&) = delete;

    /// Builds a new index with `threads` workers and atomically publishes it.
    /// Only one build may run at a time (Error::InProgress otherwise).
    /// Searches keep working on the previous index during a rebuild.
    IndexBuildResult buildIndex(std::size_t threads);

    /// Starts buildIndex() on a background thread (used for indexing at startup).
    /// `onDone` (optional) is invoked on that thread with the result.
    void buildIndexAsync(std::size_t threads, std::function<void(const IndexBuildResult&)> onDone = {});

    [[nodiscard]] bool indexing() const noexcept { return indexing_.load(); }
    [[nodiscard]] bool indexReady() const { return engine_.ready(); }

    /// Query expansion step (never throws; returns Disabled when `enabled` is false).
    ollama::ExpansionResult expandQuery(const std::string& query, bool enabled) const;

    /// Keyword search step. Throws IndexNotReadyError.
    SearchResponse keywordSearch(const std::string& query, const std::vector<std::string>& expandedTerms,
                                 std::size_t limit);

    /// Expansion followed by keyword search. Throws IndexNotReadyError.
    PipelineResult runPipeline(const std::string& query, std::size_t limit, bool expand);

    [[nodiscard]] ServiceStats stats() const;
    [[nodiscard]] bool ollamaAvailable() const;
    [[nodiscard]] std::string ollamaModel() const;

    /// Pool on which HTTP/WebSocket handlers execute search requests.
    threading::ThreadPool& requestPool() noexcept { return *requestPool_; }

    /// Small separate pool for health checks and index-build requests, so that they
    /// are never queued behind slow LLM calls in the request pool.
    threading::ThreadPool& controlPool() noexcept { return *controlPool_; }

    [[nodiscard]] const core::Config& config() const noexcept { return config_; }

    /// Stops the request pool and waits for a background index build.
    void shutdown();

private:
    core::Config config_;
    std::shared_ptr<ollama::LlmClient> llm_;
    ollama::QueryExpander expander_;
    SearchEngine engine_;
    std::unique_ptr<threading::ThreadPool> requestPool_;
    std::unique_ptr<threading::ThreadPool> controlPool_;

    std::atomic<bool> indexing_{false};
    std::atomic<std::size_t> searchesServed_{0};

    mutable std::mutex statsMutex_;
    double lastBuildTimeMs_ = 0.0;
    std::size_t lastThreads_ = 0;

    std::mutex backgroundMutex_;
    std::thread backgroundBuild_;
};

}  // namespace search::engine
