#include "api/JsonResponse.hpp"

#include <cmath>

namespace search::api {

using nlohmann::json;

drogon::HttpResponsePtr jsonResponse(const json& body, drogon::HttpStatusCode status) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(status);
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    // `replace` keeps invalid UTF-8 (e.g. from documents) from aborting serialization.
    response->setBody(body.dump(-1, ' ', false, json::error_handler_t::replace));
    return response;
}

json errorBody(const std::string& code, const std::string& message) {
    return json{{"error", code}, {"message", message}};
}

drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode status, const std::string& code,
                                      const std::string& message) {
    return jsonResponse(errorBody(code, message), status);
}

double roundMs(double ms) {
    return std::round(ms * 1000.0) / 1000.0;
}

json resultsToJson(const std::vector<engine::SearchResult>& results) {
    json array = json::array();
    for (const auto& r : results) {
        array.push_back({{"doc_id", r.docId}, {"score", r.score}, {"snippet", r.snippet}});
    }
    return array;
}

json expansionToJson(const ollama::ExpansionResult& expansion) {
    return json{
        {"original", expansion.original},
        {"expanded_terms", expansion.expandedTerms},
        {"status", ollama::toString(expansion.status)},
        {"fallback_reason",
         expansion.fallbackReason.empty() ? json(nullptr) : json(expansion.fallbackReason)},
        {"time_ms", roundMs(expansion.timeMs)},
    };
}

}  // namespace search::api
