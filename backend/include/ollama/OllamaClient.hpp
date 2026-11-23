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
/// - Availability (GET /api/tags + model present) is refreshed in the BACKGROUND every
///   `healthTtlSec` seconds by a non-blocking request on the client's own event loop,
///   so isAvailable() returns immediately and a search never waits for a health check
///   (only the very first call, before any result exists, checks synchronously).
///   With healthTtlSec == 0 every call checks synchronously (used by tests).
/// - A connection-level failure of generate() marks Ollama as unavailable, and a
///   timeout triggers an immediate re-check, so the following searches fall back
///   immediately instead of waiting for timeouts again.
class OllamaClient : public LlmClient {
public:
    OllamaClient(std::string baseUrl, std::string model, double generateTimeoutSec, double healthTimeoutSec,
                 double healthTtlSec);
    ~OllamaClient() override;

    bool isAvailable() override;
    std::optional<std::string> generate(const std::string& prompt, std::string* error,
                                        double timeoutSec = 0.0) override;
    [[nodiscard]] std::string model() const override { return model_; }

    /// Performs an uncached availability check (GET /api/tags + model lookup).
    bool checkNow();

    [[nodiscard]] const std::string& baseUrl() const noexcept { return baseUrl_; }

private:
    drogon::HttpClientPtr makeClient() const;
    drogon::HttpRequestPtr makeTagsRequest() const;
    bool interpretTags(drogon::ReqResult result, const drogon::HttpResponsePtr& response) const;
    void storeAvailability(bool available);
    void refreshAsync();  // runs on loopThread_

    std::string baseUrl_;
    std::string origin_;      // scheme://host:port
    std::string pathPrefix_;  // optional path prefix (used by tests)
    std::string model_;
    double generateTimeoutSec_;
    double healthTimeoutSec_;
    std::chrono::duration<double> healthTtl_;

    std::unique_ptr<trantor::EventLoopThread> loopThread_;

    std::mutex refreshMutex_;  // serializes availability checks
    std::mutex cacheMutex_;    // protects the cached state below
    std::chrono::steady_clock::time_point lastCheck_{};
    bool hasCache_ = false;
    bool lastAvailable_ = false;
    std::atomic<bool> refreshInFlight_{false};
};

}  // namespace search::ollama
