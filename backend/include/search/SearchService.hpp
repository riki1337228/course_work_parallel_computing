#pragma once

#include <atomic>
#include <chrono>
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
/// worker pools used to execute requests off the HTTP event loop.
/// Framework-independent: the Drogon layer only adapts HTTP/WebSocket to it.
///
/// Isolation of the slow LLM (bulkhead pattern):
///  - searches WITHOUT expansion run on `requestPool` and never wait for Ollama;
///  - searches WITH expansion run on a separate, small `llmPool` (LLM_THREADS);
///  - if more than LLM_MAX_QUEUE expansion searches are already waiting, a new one
///    is routed to `requestPool` with an immediate fallback ("LLM is overloaded");
///  - the expansion budget (OLLAMA_TIMEOUT_SEC) is counted from the request's
///    arrival, so time spent in the queue is included.
class SearchService {
public:
    using Clock = std::chrono::steady_clock;

    /// Where a search request is executed.
    enum class Route {
        Plain,       ///< no expansion requested -> requestPool
        Llm,         ///< expansion -> llmPool
        Overloaded,  ///< expansion requested but llmPool queue is full -> requestPool + fallback
    };
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

    /// Chooses the execution route for a request (see class comment).
    [[nodiscard]] Route route(bool expand) const;

    /// Pool that executes requests of the given route.
    threading::ThreadPool& poolFor(Route route) noexcept;

    /// True if the pool for `route` already holds MAX_PENDING_REQUESTS queued tasks
    /// (the request should be rejected with 503 server_busy).
    [[nodiscard]] bool saturated(Route route) const;

    /// Latest moment at which an expansion started at `arrival` may still use the LLM.
    [[nodiscard]] Clock::time_point expansionDeadline(Clock::time_point arrival) const;

    /// Query expansion step (never throws). Disabled when `enabled` is false;
    /// immediate Fallback when `overloaded` is true.
    ollama::ExpansionResult expandQuery(const std::string& query, bool enabled,
                                        Clock::time_point deadline = Clock::time_point::max(),
                                        bool overloaded = false) const;

    /// Keyword search step. Throws IndexNotReadyError.
    SearchResponse keywordSearch(const std::string& query, const std::vector<std::string>& expandedTerms,
                                 std::size_t limit);

    /// Expansion followed by keyword search. `arrival` is when the request was
    /// received (total time and the LLM budget are measured from it).
    /// Throws IndexNotReadyError.
    PipelineResult runPipeline(const std::string& query, std::size_t limit, bool expand,
                               Clock::time_point arrival = Clock::now(), bool overloaded = false);

    [[nodiscard]] ServiceStats stats() const;
    [[nodiscard]] bool ollamaAvailable() const;
    [[nodiscard]] std::string ollamaModel() const;

    /// Pool for searches without LLM expansion.
    threading::ThreadPool& requestPool() noexcept { return *requestPool_; }

    /// Separate small pool for searches that call the LLM.
    threading::ThreadPool& llmPool() noexcept { return *llmPool_; }

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
    std::unique_ptr<threading::ThreadPool> llmPool_;
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
