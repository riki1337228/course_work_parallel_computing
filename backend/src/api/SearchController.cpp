#include <drogon/drogon.h>

#include "api/Controllers.hpp"
#include "api/JsonResponse.hpp"

namespace search::api {

using nlohmann::json;

namespace {

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

}  // namespace

std::optional<SearchRequest> SearchController::parseRequest(const json& body, const core::Config& config,
                                                            ValidationError& error) {
    if (!body.is_object()) {
        error = {"invalid_json", "request body must be a JSON object"};
        return std::nullopt;
    }
    SearchRequest request;
    request.limit = config.defaultLimit;

    const auto query = body.find("query");
    if (query == body.end() || !query->is_string()) {
        error = {"invalid_query", "field 'query' (string) is required"};
        return std::nullopt;
    }
    request.query = trim(query->get<std::string>());
    if (request.query.empty()) {
        error = {"invalid_query", "query must not be empty"};
        return std::nullopt;
    }
    if (request.query.size() > config.maxQueryLength) {
        error = {"invalid_query", "query is longer than " + std::to_string(config.maxQueryLength) + " characters"};
        return std::nullopt;
    }

    if (const auto mode = body.find("mode"); mode != body.end()) {
        if (!mode->is_string() || mode->get<std::string>() != "keyword") {
            error = {"invalid_mode", "mode must be \"keyword\""};
            return std::nullopt;
        }
    }

    if (const auto limit = body.find("limit"); limit != body.end()) {
        const std::string range = "limit must be an integer between 1 and " + std::to_string(config.maxLimit);
        if (!limit->is_number_integer()) {
            error = {"invalid_limit", range};
            return std::nullopt;
        }
        const auto value = limit->get<long long>();
        if (value < 1 || value > static_cast<long long>(config.maxLimit)) {
            error = {"invalid_limit", range};
            return std::nullopt;
        }
        request.limit = static_cast<std::size_t>(value);
    }

    if (const auto expand = body.find("expand"); expand != body.end()) {
        if (!expand->is_boolean()) {
            error = {"invalid_expand", "expand must be a boolean"};
            return std::nullopt;
        }
        request.expand = expand->get<bool>();
    }
    return request;
}

void SearchController::handle(const drogon::HttpRequestPtr& request, Callback&& callback) const {
    const json body = json::parse(request->body(), nullptr, false);
    if (body.is_discarded()) {
        callback(errorResponse(drogon::k400BadRequest, "invalid_json", "request body is not valid JSON"));
        return;
    }
    ValidationError error;
    const auto parsed = parseRequest(body, service_->config(), error);
    if (!parsed) {
        LOG_DEBUG << "POST /api/search rejected: " << error.message;
        callback(errorResponse(drogon::k400BadRequest, error.code, error.message));
        return;
    }
    if (!service_->indexReady()) {
        callback(errorResponse(drogon::k503ServiceUnavailable, "index_not_ready",
                               service_->indexing() ? "index is being built, try again later"
                                                    : "index is not built; call POST /api/index"));
        return;
    }

    auto service = service_;
    auto respond = std::make_shared<Callback>(std::move(callback));
    try {
        // Expansion (LLM call) and search are blocking -> executed on the request pool,
        // the Drogon event loop is released immediately.
        service->requestPool().post([service, respond, req = *parsed] {
            try {
                const auto result = service->runPipeline(req.query, req.limit, req.expand);
                if (result.expansion.status == ollama::ExpansionStatus::Fallback) {
                    LOG_WARN << "Query expansion fallback for \"" << req.query
                             << "\": " << result.expansion.fallbackReason;
                }
                LOG_INFO << "POST /api/search \"" << req.query << "\" expansion="
                         << ollama::toString(result.expansion.status) << " matches=" << result.search.totalMatches
                         << " time=" << result.totalTimeMs << " ms";
                (*respond)(jsonResponse({
                    {"results", resultsToJson(result.search.results)},
                    {"time_ms", roundMs(result.totalTimeMs)},
                    {"total_matches", result.search.totalMatches},
                    {"query", req.query},
                    {"expanded_terms", result.expansion.expandedTerms},
                    {"expansion_status", ollama::toString(result.expansion.status)},
                    {"fallback_reason", result.expansion.fallbackReason.empty()
                                            ? json(nullptr)
                                            : json(result.expansion.fallbackReason)},
                    {"search_time_ms", roundMs(result.search.timeMs)},
                    {"expansion_time_ms", roundMs(result.expansion.timeMs)},
                }));
            } catch (const engine::IndexNotReadyError& e) {
                (*respond)(errorResponse(drogon::k503ServiceUnavailable, "index_not_ready", e.what()));
            } catch (const std::exception& e) {
                LOG_ERROR << "search failed: " << e.what();
                (*respond)(errorResponse(drogon::k500InternalServerError, "internal_error", e.what()));
            }
        });
    } catch (const std::exception& e) {
        (*respond)(errorResponse(drogon::k503ServiceUnavailable, "server_shutting_down", e.what()));
    }
}

}  // namespace search::api
