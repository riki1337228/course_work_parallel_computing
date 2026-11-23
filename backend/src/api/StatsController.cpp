#include "api/Controllers.hpp"
#include "api/JsonResponse.hpp"

namespace search::api {

void StatsController::handle(const drogon::HttpRequestPtr& /*request*/, Callback&& callback) const {
    // Cheap: reads counters and takes short shared locks; safe on the event loop.
    const auto stats = service_->stats();
    callback(jsonResponse({
        {"documents", stats.documents},
        {"index_build_time_ms", roundMs(stats.indexBuildTimeMs)},
        {"threads_used", stats.threadsUsed},
        {"unique_terms", stats.uniqueTerms},
        {"total_postings", stats.totalPostings},
        {"index_ready", stats.indexReady},
        {"indexing_in_progress", stats.indexingInProgress},
        {"searches_served", stats.searchesServed},
        {"ollama_model", service_->ollamaModel()},
    }));
}

}  // namespace search::api
