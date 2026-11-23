#include "ollama/OllamaClient.hpp"

#include <algorithm>
#include <cmath>
#include <future>
#include <utility>

#include <drogon/drogon.h>

#include "ollama/OllamaProtocol.hpp"

namespace search::ollama {
namespace {

std::string describe(drogon::ReqResult result) {
    switch (result) {
        case drogon::ReqResult::Ok:
            return "ok";
        case drogon::ReqResult::BadResponse:
            return "bad response";
        case drogon::ReqResult::NetworkFailure:
            return "network failure";
        case drogon::ReqResult::BadServerAddress:
            return "bad server address";
        case drogon::ReqResult::Timeout:
            return "timeout";
        case drogon::ReqResult::HandshakeError:
            return "TLS handshake error";
        case drogon::ReqResult::InvalidCertificate:
            return "invalid certificate";
        default:
            return "request error";
    }
}

}  // namespace

OllamaClient::OllamaClient(std::string baseUrl, std::string model, double generateTimeoutSec,
                           double healthTimeoutSec, double healthTtlSec)
    : baseUrl_(std::move(baseUrl)),
      model_(std::move(model)),
      generateTimeoutSec_(generateTimeoutSec),
      healthTimeoutSec_(healthTimeoutSec),
      healthTtl_(healthTtlSec),
      loopThread_(std::make_unique<trantor::EventLoopThread>("OllamaClientLoop")) {
    while (!baseUrl_.empty() && baseUrl_.back() == '/') {
        baseUrl_.pop_back();
    }
    // Split "http://host:port/prefix" into origin and path prefix.
    const auto schemeEnd = baseUrl_.find("://");
    const auto pathStart = baseUrl_.find('/', schemeEnd == std::string::npos ? 0 : schemeEnd + 3);
    origin_ = pathStart == std::string::npos ? baseUrl_ : baseUrl_.substr(0, pathStart);
    pathPrefix_ = pathStart == std::string::npos ? std::string{} : baseUrl_.substr(pathStart);
    loopThread_->run();

    if (healthTtl_.count() > 0) {
        // Periodic, non-blocking availability refresh on the client's own loop.
        auto* loop = loopThread_->getLoop();
        loop->runInLoop([this] { refreshAsync(); });
        loop->runEvery(healthTtl_.count(), [this] { refreshAsync(); });
    }
}

OllamaClient::~OllamaClient() {
    // Stop the event loop (and its timers/callbacks that capture `this`) before any
    // other member is destroyed.
    loopThread_.reset();
}

drogon::HttpClientPtr OllamaClient::makeClient() const {
    // A fresh client per request: independent connections, so concurrent searches
    // do not queue behind each other on one keep-alive connection.
    return drogon::HttpClient::newHttpClient(origin_, loopThread_->getLoop());
}

drogon::HttpRequestPtr OllamaClient::makeTagsRequest() const {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(pathPrefix_ + "/api/tags");
    return request;
}

bool OllamaClient::interpretTags(drogon::ReqResult result, const drogon::HttpResponsePtr& response) const {
    return result == drogon::ReqResult::Ok && response && response->getStatusCode() == drogon::k200OK &&
           OllamaProtocol::modelListed(std::string(response->body()), model_);
}

void OllamaClient::storeAvailability(bool available) {
    std::lock_guard lock(cacheMutex_);
    lastAvailable_ = available;
    lastCheck_ = std::chrono::steady_clock::now();
    hasCache_ = true;
}

void OllamaClient::refreshAsync() {
    bool expected = false;
    if (!refreshInFlight_.compare_exchange_strong(expected, true)) {
        return;  // previous check still running
    }
    auto client = makeClient();
    client->sendRequest(
        makeTagsRequest(),
        [this, client](drogon::ReqResult result, const drogon::HttpResponsePtr& response) {
            storeAvailability(interpretTags(result, response));
            refreshInFlight_.store(false);
        },
        healthTimeoutSec_);
}

bool OllamaClient::checkNow() {
    auto client = makeClient();
    const auto [result, response] = client->sendRequest(makeTagsRequest(), healthTimeoutSec_);
    const bool available = interpretTags(result, response);
    storeAvailability(available);
    return available;
}

bool OllamaClient::isAvailable() {
    {
        std::lock_guard lock(cacheMutex_);
        const bool fresh = std::chrono::steady_clock::now() - lastCheck_ < healthTtl_;
        // With background refresh enabled any cached value is used immediately;
        // without it (TTL == 0) only a fresh one.
        if (hasCache_ && (healthTtl_.count() > 0 || fresh)) {
            return lastAvailable_;
        }
    }
    // No result yet (or TTL == 0): one synchronous check, serialized between threads.
    std::lock_guard refresh(refreshMutex_);
    if (healthTtl_.count() > 0) {
        std::lock_guard lock(cacheMutex_);
        if (hasCache_) {
            return lastAvailable_;
        }
    }
    try {
        return checkNow();
    } catch (const std::exception& e) {
        LOG_WARN << "Ollama availability check failed: " << e.what();
        storeAvailability(false);
        return false;
    }
}

std::optional<std::string> OllamaClient::generate(const std::string& prompt, std::string* error,
                                                  double timeoutSec) {
    auto setError = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
    };
    const double timeout = timeoutSec > 0.0 ? std::min(timeoutSec, generateTimeoutSec_) : generateTimeoutSec_;
    auto client = makeClient();
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setPath(pathPrefix_ + "/api/generate");
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->setBody(OllamaProtocol::buildGenerateRequest(model_, prompt));

    // Asynchronous request + bounded waiting: while waiting, a fresh availability
    // probe runs in parallel. If it reports that Ollama has gone away (e.g. the
    // container was stopped and DNS/connect would only fail at the timeout), the
    // wait is abandoned right away instead of blocking for the whole budget.
    using Reply = std::pair<drogon::ReqResult, drogon::HttpResponsePtr>;
    auto promise = std::make_shared<std::promise<Reply>>();
    auto future = promise->get_future();
    const auto started = std::chrono::steady_clock::now();
    client->sendRequest(
        request,
        [promise, client](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) { promise->set_value({r, resp}); },
        timeout);
    if (healthTtl_.count() > 0) {
        loopThread_->getLoop()->queueInLoop([this] { refreshAsync(); });
    }
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        std::lock_guard lock(cacheMutex_);
        if (hasCache_ && !lastAvailable_ && lastCheck_ > started) {
            setError("Ollama became unavailable while waiting for the answer");
            return std::nullopt;  // the pending request is abandoned; it ends at its timeout
        }
    }
    const auto [result, response] = future.get();
    if (result != drogon::ReqResult::Ok || !response) {
        if (result == drogon::ReqResult::Timeout) {
            // Busy model or dead host? Re-check so the next searches know.
            std::lock_guard refresh(refreshMutex_);
            checkNow();
            setError("Ollama did not answer within " + std::to_string(static_cast<int>(std::lround(timeout))) +
                     " s");
        } else {
            storeAvailability(false);  // connection-level failure: fail fast next time
            setError("Ollama request failed: " + describe(result));
        }
        return std::nullopt;
    }
    if (response->getStatusCode() != drogon::k200OK) {
        std::string body(response->body());
        if (body.size() > 200) body.resize(200);
        setError("Ollama HTTP " + std::to_string(static_cast<int>(response->getStatusCode())) + ": " + body);
        return std::nullopt;
    }
    std::string parseError;
    auto text = OllamaProtocol::extractResponseText(std::string(response->body()), &parseError);
    if (!text) {
        setError(parseError);
    }
    return text;
}

}  // namespace search::ollama
