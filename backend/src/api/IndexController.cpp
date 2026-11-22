#include <drogon/drogon.h>

#include "api/Controllers.hpp"
#include "api/JsonResponse.hpp"

namespace search::api {

using nlohmann::json;

void IndexController::handle(const drogon::HttpRequestPtr& request, Callback&& callback) const {
    const auto& config = service_->config();
    std::size_t threads = config.indexThreads;

    const std::string_view rawBody = request->body();
    if (!rawBody.empty()) {
        const json body = json::parse(rawBody, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            callback(errorResponse(drogon::k400BadRequest, "invalid_json", "request body must be a JSON object"));
            return;
        }
        if (const auto it = body.find("threads"); it != body.end()) {
            const std::string range = "threads must be an integer between 1 and " +
                                      std::to_string(config.maxIndexThreads);
            if (!it->is_number_integer()) {
                callback(errorResponse(drogon::k400BadRequest, "invalid_threads", range));
                return;
            }
            const auto value = it->get<long long>();
            if (value < 1 || value > static_cast<long long>(config.maxIndexThreads)) {
                callback(errorResponse(drogon::k400BadRequest, "invalid_threads", range));
                return;
            }
            threads = static_cast<std::size_t>(value);
        }
    }

    if (service_->indexing()) {  // fast path; buildIndex() re-checks atomically
        callback(errorResponse(drogon::k409Conflict, "indexing_in_progress", "index build is already in progress"));
        return;
    }

    auto service = service_;
    auto respond = std::make_shared<Callback>(std::move(callback));
    try {
        service->controlPool().post([service, respond, threads] {
            LOG_INFO << "POST /api/index: building index with " << threads << " threads";
            const auto result = service->buildIndex(threads);
            using Error = engine::IndexBuildResult::Error;
            switch (result.error) {
                case Error::None:
                    LOG_INFO << "Index built: " << result.documentsIndexed << " documents in " << result.timeMs
                             << " ms (threads=" << threads << ", tasks=" << result.stats.tasks
                             << ", read=" << result.stats.bytesRead / (1024 * 1024) << " MiB)";
                    (*respond)(jsonResponse({
                        {"status", "ok"},
                        {"documents_indexed", result.documentsIndexed},
                        {"time_ms", roundMs(result.timeMs)},
                        {"threads_used", threads},
                    }));
                    return;
                case Error::InProgress:
                    (*respond)(errorResponse(drogon::k409Conflict, "indexing_in_progress", result.message));
                    return;
                case Error::DatasetEmpty:
                    LOG_ERROR << result.message;
                    (*respond)(errorResponse(drogon::k422UnprocessableEntity, "dataset_empty", result.message));
                    return;
                case Error::DatasetNotFound:
                    LOG_ERROR << result.message;
                    (*respond)(errorResponse(drogon::k500InternalServerError, "dataset_not_found", result.message));
                    return;
                case Error::Failed:
                    LOG_ERROR << result.message;
                    (*respond)(errorResponse(drogon::k500InternalServerError, "index_failed", result.message));
                    return;
            }
        });
    } catch (const std::exception& e) {
        (*respond)(errorResponse(drogon::k503ServiceUnavailable, "server_shutting_down", e.what()));
    }
}

}  // namespace search::api
