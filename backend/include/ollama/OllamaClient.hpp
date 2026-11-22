#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <drogon/HttpClient.h>
#include <trantor/net/EventLoopThread.h>

#include "ollama/LlmClient.hpp"

namespace search::ollama {

/// HTTP client for a local Ollama server (POST /api/generate, GET /api/tags).
///
/// - Requests run on a dedicated event-loop thread owned by the client, so the
///   blocking generate() may be called from any worker thread (never from that loop).
/// - Every request has a timeout; generate() can never block forever.
/// - isAvailable() caches the result for `healthTtlSec` seconds and only one thread
///   refreshes it at a time (others get the cached value) -> no request storms.
/// - A connection-level failure of generate() marks Ollama as unavailable, so the
///   following searches fall back immediately instead of waiting for timeouts.
class OllamaClient : public LlmClient {
public:
    OllamaClient(std::string baseUrl, std::string model, double generateTimeoutSec, double healthTimeoutSec,
                 double healthTtlSec);
    ~OllamaClient() override;

    bool isAvailable() override;
    std::optional<std::string> generate(const std::string& prompt, std::string* error) override;
    [[nodiscard]] std::string model() const override { return model_; }

    /// Performs an uncached availability check (GET /api/tags + model lookup).
    bool checkNow();

    [[nodiscard]] const std::string& baseUrl() const noexcept { return baseUrl_; }

private:
    drogon::HttpClientPtr makeClient() const;
    void storeAvailability(bool available);

    std::string baseUrl_;
    std::string origin_;      // scheme://host:port
    std::string pathPrefix_;  // optional path prefix (used by tests)
    std::string model_;
    double generateTimeoutSec_;
    double healthTimeoutSec_;
    std::chrono::duration<double> healthTtl_;

    std::unique_ptr<trantor::EventLoopThread> loopThread_;

    std::mutex cacheMutex_;
    std::chrono::steady_clock::time_point lastCheck_{};
    bool hasCache_ = false;
    bool lastAvailable_ = false;
    std::atomic<bool> refreshing_{false};
};

}  // namespace search::ollama
