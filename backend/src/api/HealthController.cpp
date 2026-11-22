#include <drogon/drogon.h>

#include "api/Controllers.hpp"
#include "api/JsonResponse.hpp"

namespace search::api {

void HealthController::handle(const drogon::HttpRequestPtr& /*request*/, Callback&& callback) const {
    auto service = service_;
    auto respond = std::make_shared<Callback>(std::move(callback));
    try {
        // The Ollama check may perform an HTTP request (bounded by
        // OLLAMA_HEALTH_TIMEOUT_SEC), so it must not run on the event loop.
        service->controlPool().post([service, respond] {
            const bool ollama = service->ollamaAvailable();
            (*respond)(jsonResponse({
                {"status", "ok"},
                {"ollama", ollama},
                {"index_ready", service->indexReady()},
                {"model", service->ollamaModel()},
            }));
        });
    } catch (const std::exception& e) {
        (*respond)(errorResponse(drogon::k503ServiceUnavailable, "server_shutting_down", e.what()));
    }
}

}  // namespace search::api
