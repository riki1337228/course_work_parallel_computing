#include "ollama/OllamaClient.hpp"

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
}

// trantor::EventLoopThread's destructor quits the loop and joins the thread.
OllamaClient::~OllamaClient() = default;

drogon::HttpClientPtr OllamaClient::makeClient() const {
    // A fresh client per request: independent connections, so concurrent searches
    // do not queue behind each other on one keep-alive connection.
    return drogon::HttpClient::newHttpClient(origin_, loopThread_->getLoop());
}

void OllamaClient::storeAvailability(bool available) {
    std::lock_guard lock(cacheMutex_);
    lastAvailable_ = available;
    lastCheck_ = std::chrono::steady_clock::now();
    hasCache_ = true;
}

bool OllamaClient::checkNow() {
    auto client = makeClient();
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(pathPrefix_ + "/api/tags");
    const auto [result, response] = client->sendRequest(request, healthTimeoutSec_);
    const bool available = result == drogon::ReqResult::Ok && response &&
                           response->getStatusCode() == drogon::k200OK &&
                           OllamaProtocol::modelListed(std::string(response->body()), model_);
    storeAvailability(available);
    return available;
}

bool OllamaClient::isAvailable() {
    {
        std::lock_guard lock(cacheMutex_);
        if (hasCache_ && std::chrono::steady_clock::now() - lastCheck_ < healthTtl_) {
            return lastAvailable_;
        }
    }
    bool expected = false;
    if (!refreshing_.compare_exchange_strong(expected, true)) {
        // Another thread is refreshing: return the last known state (false if none).
        std::lock_guard lock(cacheMutex_);
        return hasCache_ && lastAvailable_;
    }
    bool available = false;
    try {
        available = checkNow();
    } catch (const std::exception& e) {
        LOG_WARN << "Ollama availability check failed: " << e.what();
        storeAvailability(false);
    }
    refreshing_.store(false);
    return available;
}

std::optional<std::string> OllamaClient::generate(const std::string& prompt, std::string* error) {
    auto setError = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
    };
    auto client = makeClient();
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setPath(pathPrefix_ + "/api/generate");
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->setBody(OllamaProtocol::buildGenerateRequest(model_, prompt));

    const auto [result, response] = client->sendRequest(request, generateTimeoutSec_);
    if (result != drogon::ReqResult::Ok || !response) {
        const std::string reason = "Ollama request failed: " + describe(result);
        if (result != drogon::ReqResult::Timeout) {
            storeAvailability(false);  // connection-level failure: fail fast next time
        }
        setError(result == drogon::ReqResult::Timeout
                     ? "Ollama did not answer within " + std::to_string(static_cast<int>(generateTimeoutSec_)) + " s"
                     : reason);
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
