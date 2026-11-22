// Search backend entry point: configures Drogon (REST + WebSocket + static Web UI),
// creates the SearchService and starts the initial parallel index build.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>

#include <drogon/drogon.h>

#include "api/Controllers.hpp"
#include "core/Config.hpp"
#include "ollama/OllamaClient.hpp"
#include "search/SearchService.hpp"

namespace {

trantor::Logger::LogLevel parseLogLevel(const std::string& level) {
    if (level == "TRACE") return trantor::Logger::kTrace;
    if (level == "DEBUG") return trantor::Logger::kDebug;
    if (level == "WARN") return trantor::Logger::kWarn;
    if (level == "ERROR") return trantor::Logger::kError;
    return trantor::Logger::kInfo;
}

}  // namespace

int main() {
    using search::core::Config;
    const Config config = Config::fromEnvironment();
    // stdout is a pipe inside Docker: flush every log line so `docker compose logs` is live.
    trantor::Logger::setOutputFunction(
        [](const char* message, std::uint64_t length) {
            std::fwrite(message, 1, static_cast<std::size_t>(length), stdout);
            std::fflush(stdout);
        },
        [] { std::fflush(stdout); });
    trantor::Logger::setLogLevel(parseLogLevel(config.logLevel));

    LOG_INFO << "Starting search server: dataset=" << config.datasetPath << " port=" << config.port
             << " io_threads=" << config.ioThreads << " request_threads=" << config.requestThreads
             << " ollama=" << config.ollamaUrl << " model=" << config.ollamaModel;

    auto ollama = std::make_shared<search::ollama::OllamaClient>(config.ollamaUrl, config.ollamaModel,
                                                                 config.ollamaTimeoutSec,
                                                                 config.ollamaHealthTimeoutSec,
                                                                 config.ollamaHealthTtlSec);
    auto service = std::make_shared<search::engine::SearchService>(config, ollama);
    search::api::registerRoutes(service);

    auto& app = drogon::app();
    if (std::filesystem::is_directory(config.frontendPath)) {
        app.setDocumentRoot(config.frontendPath);
    } else {
        LOG_WARN << "Frontend directory not found: " << config.frontendPath << " (Web UI disabled)";
    }
    app.addListener(config.listenAddress, config.port)
        .setThreadNum(config.ioThreads)
        .setClientMaxBodySize(64 * 1024)
        .setLogLevel(parseLogLevel(config.logLevel))
        .setIdleConnectionTimeout(120);

    if (config.autoIndex) {
        // Indexing starts as soon as the event loop is running (the dataset is
        // guaranteed to be ready: docker compose waits for dataset-init).
        app.registerBeginningAdvice([service, threads = config.indexThreads] {
            LOG_INFO << "Auto-indexing dataset with " << threads << " threads...";
            service->buildIndexAsync(threads, [threads](const search::engine::IndexBuildResult& r) {
                if (r.error == search::engine::IndexBuildResult::Error::None) {
                    LOG_INFO << "Auto-indexing finished: " << r.documentsIndexed << " documents, " << r.timeMs
                             << " ms, threads=" << threads << ", unreadable=" << r.stats.documentsFailed;
                } else {
                    LOG_ERROR << "Auto-indexing failed: " << r.message;
                }
            });
        });
    }

    app.run();  // blocks until SIGINT/SIGTERM

    LOG_INFO << "Shutting down...";
    service->shutdown();
    return 0;
}
