#pragma once

#include <memory>

#include <drogon/WebSocketController.h>
#include <nlohmann/json.hpp>

#include "search/SearchService.hpp"

namespace search::api {

/// WebSocket endpoint /ws/search.
///
/// Client -> server: {"query": string, "limit"?: int, "expand"?: bool}
/// Server -> client (separate text frames, strictly in this order):
///   {"stage": "query_expansion", "data": {...}}
///   {"stage": "keyword_search",  "data": {...}}
///   {"stage": "done"}
/// On invalid input / failure: {"stage": "error", "data": {"error": code, "message": text}}.
///
/// Each message is processed on the service request pool (never on the event loop);
/// the three frames are sent sequentially from one task, so their order is guaranteed.
/// One search per connection at a time; connections are fully independent.
class SearchWebSocket : public drogon::WebSocketController<SearchWebSocket> {
public:
    void handleNewMessage(const drogon::WebSocketConnectionPtr& connection, std::string&& message,
                          const drogon::WebSocketMessageType& type) override;
    void handleNewConnection(const drogon::HttpRequestPtr& request,
                             const drogon::WebSocketConnectionPtr& connection) override;
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& connection) override;

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/search");
    WS_PATH_LIST_END

    /// Must be called once before the server starts.
    static void setService(std::shared_ptr<engine::SearchService> service);

    static nlohmann::json stageMessage(const std::string& stage, const nlohmann::json& data);
    static nlohmann::json errorMessage(const std::string& code, const std::string& message);

private:
    static std::shared_ptr<engine::SearchService>& service();
};

}  // namespace search::api
