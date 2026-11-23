#include <drogon/drogon.h>

#include "api/Controllers.hpp"
#include "api/JsonResponse.hpp"
#include "api/SearchWebSocket.hpp"

namespace search::api {

void registerRoutes(const std::shared_ptr<engine::SearchService>& service) {
    auto& app = drogon::app();

    auto health = std::make_shared<HealthController>(service);
    auto index = std::make_shared<IndexController>(service);
    auto search = std::make_shared<SearchController>(service);
    auto stats = std::make_shared<StatsController>(service);

    app.registerHandler(
        "/api/health",
        [health](const drogon::HttpRequestPtr& req, Callback&& cb) { health->handle(req, std::move(cb)); },
        {drogon::Get});
    app.registerHandler(
        "/api/index",
        [index](const drogon::HttpRequestPtr& req, Callback&& cb) { index->handle(req, std::move(cb)); },
        {drogon::Post});
    app.registerHandler(
        "/api/search",
        [search](const drogon::HttpRequestPtr& req, Callback&& cb) { search->handle(req, std::move(cb)); },
        {drogon::Post});
    app.registerHandler(
        "/api/stats",
        [stats](const drogon::HttpRequestPtr& req, Callback&& cb) { stats->handle(req, std::move(cb)); },
        {drogon::Get});

    // Unknown paths (outside the static Web UI) get a JSON 404.
    app.setCustom404Page(errorResponse(drogon::k404NotFound, "not_found", "resource not found"), false);

    // Drogon answers a wrong HTTP method with an empty 405; make it a JSON error too.
    app.registerPreSendingAdvice([](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
        if (resp->statusCode() == drogon::k405MethodNotAllowed && resp->body().empty()) {
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(errorBody("method_not_allowed",
                                    std::string("method ") + req->methodString() + " is not allowed for " +
                                        req->path())
                              .dump());
        }
    });

    SearchWebSocket::setService(service);
}

}  // namespace search::api
