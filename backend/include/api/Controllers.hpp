#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

#include "search/SearchService.hpp"

namespace search::api {

using Callback = std::function<void(const drogon::HttpResponsePtr&)>;

/// Base class: every controller works on the shared SearchService.
class ControllerBase {
public:
    explicit ControllerBase(std::shared_ptr<engine::SearchService> service) : service_(std::move(service)) {}
    virtual ~ControllerBase() = default;

protected:
    std::shared_ptr<engine::SearchService> service_;
};

/// GET /api/health -> {"status": "ok", "ollama": bool}
class HealthController : public ControllerBase {
public:
    using ControllerBase::ControllerBase;
    void handle(const drogon::HttpRequestPtr& request, Callback&& callback) const;
};

/// POST /api/index {"threads": int} -> {"status", "documents_indexed", "time_ms", "threads_used"}
class IndexController : public ControllerBase {
public:
    using ControllerBase::ControllerBase;
    void handle(const drogon::HttpRequestPtr& request, Callback&& callback) const;
};

/// GET /api/stats -> {"documents", "index_build_time_ms", "threads_used", ...}
class StatsController : public ControllerBase {
public:
    using ControllerBase::ControllerBase;
    void handle(const drogon::HttpRequestPtr& request, Callback&& callback) const;
};

/// Validated search request (shared by REST and WebSocket).
struct SearchRequest {
    std::string query;
    std::size_t limit = 10;
    bool expand = true;
};

/// Validation error: machine-readable code + message (HTTP 400).
struct ValidationError {
    std::string code;
    std::string message;
};

/// POST /api/search {"query", "mode", "limit"?, "expand"?}
class SearchController : public ControllerBase {
public:
    using ControllerBase::ControllerBase;
    void handle(const drogon::HttpRequestPtr& request, Callback&& callback) const;

    /// Validates a JSON request object. On failure fills `error` and returns nullopt.
    /// `requireMode`: REST requires "mode":"keyword" (API contract); the WebSocket
    /// contract only has "query", so there "mode" is optional.
    static std::optional<SearchRequest> parseRequest(const nlohmann::json& body, const core::Config& config,
                                                     ValidationError& error, bool requireMode);
};

/// Registers all REST routes, the JSON 404 handler and the WebSocket service.
void registerRoutes(const std::shared_ptr<engine::SearchService>& service);

}  // namespace search::api
