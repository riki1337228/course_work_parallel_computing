#pragma once

#include <string>

#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

#include "search/SearchService.hpp"

namespace search::api {

/// Serializes `body` as application/json with the given status code.
drogon::HttpResponsePtr jsonResponse(const nlohmann::json& body, drogon::HttpStatusCode status = drogon::k200OK);

/// Uniform error body: {"error": "<code>", "message": "<human readable text>"}.
nlohmann::json errorBody(const std::string& code, const std::string& message);

drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode status, const std::string& code,
                                      const std::string& message);

/// Rounds milliseconds to 3 decimals for compact, readable JSON numbers.
double roundMs(double ms);

/// {"doc_id": ..., "score": ..., "snippet": ...} array.
nlohmann::json resultsToJson(const std::vector<engine::SearchResult>& results);

/// {"original", "expanded_terms", "status", "fallback_reason", "time_ms"}.
nlohmann::json expansionToJson(const ollama::ExpansionResult& expansion);

}  // namespace search::api
