#include "api/SearchWebSocket.hpp"

#include <atomic>

#include <drogon/drogon.h>

#include "api/Controllers.hpp"
#include "api/JsonResponse.hpp"

namespace search::api {

using nlohmann::json;

namespace {

/// Per-connection state stored in the connection context.
struct ConnectionState {
    std::atomic<bool> busy{false};
};

void sendJson(const drogon::WebSocketConnectionPtr& connection, const json& message) {
    if (connection->connected()) {
        connection->send(message.dump(-1, ' ', false, json::error_handler_t::replace));
    }
}

}  // namespace

std::shared_ptr<engine::SearchService>& SearchWebSocket::service() {
    static std::shared_ptr<engine::SearchService> instance;
    return instance;
}

void SearchWebSocket::setService(std::shared_ptr<engine::SearchService> service) {
    SearchWebSocket::service() = std::move(service);
}

json SearchWebSocket::stageMessage(const std::string& stage, const json& data) {
    json message = {{"stage", stage}};
    if (!data.is_null()) {
        message["data"] = data;
    }
    return message;
}

json SearchWebSocket::errorMessage(const std::string& code, const std::string& message) {
    return stageMessage("error", errorBody(code, message));
}

void SearchWebSocket::handleNewConnection(const drogon::HttpRequestPtr& request,
                                          const drogon::WebSocketConnectionPtr& connection) {
    connection->setContext(std::make_shared<ConnectionState>());
    LOG_INFO << "WebSocket connected: " << request->peerAddr().toIpPort();
}

void SearchWebSocket::handleConnectionClosed(const drogon::WebSocketConnectionPtr& connection) {
    LOG_INFO << "WebSocket closed: " << connection->peerAddr().toIpPort();
}

void SearchWebSocket::handleNewMessage(const drogon::WebSocketConnectionPtr& connection, std::string&& message,
                                       const drogon::WebSocketMessageType& type) {
    if (type == drogon::WebSocketMessageType::Ping || type == drogon::WebSocketMessageType::Pong ||
        type == drogon::WebSocketMessageType::Close) {
        return;
    }
    auto svc = service();
    if (!svc) {
        sendJson(connection, errorMessage("internal_error", "service is not initialized"));
        return;
    }
    if (type != drogon::WebSocketMessageType::Text) {
        sendJson(connection, errorMessage("invalid_message", "only text JSON messages are supported"));
        return;
    }

    const json body = json::parse(message, nullptr, false);
    if (body.is_discarded()) {
        sendJson(connection, errorMessage("invalid_json", "message is not valid JSON"));
        return;
    }
    ValidationError error;
    const auto arrival = engine::SearchService::Clock::now();
    const auto request = SearchController::parseRequest(body, svc->config(), error, /*requireMode=*/false);
    if (!request) {
        sendJson(connection, errorMessage(error.code, error.message));
        return;
    }
    if (!svc->indexReady()) {
        sendJson(connection, errorMessage("index_not_ready", svc->indexing() ? "index is being built, try again later"
                                                                             : "index is not built"));
        return;
    }

    const auto route = svc->route(request->expand);
    if (svc->saturated(route)) {
        sendJson(connection, errorMessage("server_busy", "too many queued requests, try again later"));
        return;
    }

    auto state = connection->getContext<ConnectionState>();
    if (!state) {
        state = std::make_shared<ConnectionState>();
        connection->setContext(state);
    }
    bool expected = false;
    if (!state->busy.compare_exchange_strong(expected, true)) {
        sendJson(connection, errorMessage("search_in_progress", "wait for the current search to finish"));
        return;
    }

    try {
        const bool overloaded = route == engine::SearchService::Route::Overloaded;
        svc->poolFor(route).post([svc, connection, state, req = *request, arrival, overloaded] {
            struct BusyGuard {
                std::atomic<bool>& flag;
                ~BusyGuard() { flag.store(false); }
            } guard{state->busy};
            try {
                // Stage 1: query expansion through Ollama (or fallback).
                const auto expansion =
                    svc->expandQuery(req.query, req.expand, svc->expansionDeadline(arrival), overloaded);
                if (expansion.status == ollama::ExpansionStatus::Fallback) {
                    LOG_WARN << "Query expansion fallback for \"" << req.query << "\": " << expansion.fallbackReason;
                }
                sendJson(connection, stageMessage("query_expansion", expansionToJson(expansion)));

                // Stage 2: keyword search in the inverted index.
                const auto response = svc->keywordSearch(req.query, expansion.expandedTerms, req.limit);
                sendJson(connection, stageMessage("keyword_search", {
                                                                        {"results", resultsToJson(response.results)},
                                                                        {"time_ms", roundMs(response.timeMs)},
                                                                        {"total_matches", response.totalMatches},
                                                                    }));
                // Stage 3: done.
                sendJson(connection, stageMessage("done", nullptr));
                LOG_INFO << "WS search \"" << req.query << "\" expansion=" << ollama::toString(expansion.status)
                         << " matches=" << response.totalMatches;
            } catch (const engine::IndexNotReadyError& e) {
                sendJson(connection, errorMessage("index_not_ready", e.what()));
            } catch (const std::exception& e) {
                LOG_ERROR << "WS search failed: " << e.what();
                sendJson(connection, errorMessage("internal_error", e.what()));
            }
        });
    } catch (const std::exception& e) {
        state->busy.store(false);
        sendJson(connection, errorMessage("server_shutting_down", e.what()));
    }
}

}  // namespace search::api
