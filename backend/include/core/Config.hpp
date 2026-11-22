#pragma once

#include <cstddef>
#include <string>

namespace search::core {

/// Runtime configuration of the search backend.
/// Every field can be overridden with an environment variable (see fromEnvironment()).
struct Config {
    // Dataset
    std::string datasetPath = "/data";          ///< DATASET_PATH: dataset root (contains documents/)

    // HTTP server
    std::string listenAddress = "0.0.0.0";      ///< LISTEN_ADDRESS
    unsigned short port = 8080;                 ///< SERVER_PORT
    std::size_t ioThreads = 4;                  ///< IO_THREADS: Drogon event-loop threads
    std::size_t requestThreads = 16;            ///< REQUEST_THREADS: worker pool for search requests
    std::string frontendPath = "/app/frontend"; ///< FRONTEND_PATH: static Web UI files

    // Indexing
    bool autoIndex = true;                      ///< AUTO_INDEX: build index on startup
    std::size_t indexThreads = 4;               ///< INDEX_THREADS: threads for startup indexing
    std::size_t maxIndexThreads = 64;           ///< MAX_INDEX_THREADS: upper bound for POST /api/index

    // Search
    std::size_t defaultLimit = 10;              ///< DEFAULT_LIMIT
    std::size_t maxLimit = 100;                 ///< MAX_LIMIT
    std::size_t maxQueryLength = 1000;          ///< MAX_QUERY_LENGTH (characters)
    double expandedTermWeight = 0.5;            ///< EXPANDED_TERM_WEIGHT: weight of LLM terms in ranking

    // Ollama
    std::string ollamaUrl = "http://ollama:11434"; ///< OLLAMA_URL
    std::string ollamaModel = "qwen2.5:3b";        ///< OLLAMA_MODEL
    double ollamaTimeoutSec = 60.0;                ///< OLLAMA_TIMEOUT_SEC: generate() timeout
    double ollamaHealthTimeoutSec = 2.0;           ///< OLLAMA_HEALTH_TIMEOUT_SEC
    double ollamaHealthTtlSec = 5.0;               ///< OLLAMA_HEALTH_TTL_SEC: availability cache TTL
    std::size_t maxExpansionTerms = 6;             ///< MAX_EXPANSION_TERMS

    std::string logLevel = "INFO";              ///< LOG_LEVEL: TRACE|DEBUG|INFO|WARN|ERROR

    /// Builds a configuration from defaults overridden by environment variables.
    static Config fromEnvironment();
};

}  // namespace search::core
