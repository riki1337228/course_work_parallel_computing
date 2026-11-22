// Integration tests: the real Drogon server (REST + WebSocket + static files) runs
// in-process on a test dataset. Ollama is replaced by a mock registered on the same
// server under /mock-ollama, whose behaviour (up / down / garbage / slow) is switched
// per test. A second OllamaClient points to a closed port (network failure case).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include <drogon/HttpClient.h>
#include <drogon/WebSocketClient.h>
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <trantor/net/EventLoopThread.h>

#include "api/Controllers.hpp"
#include "ollama/OllamaClient.hpp"
#include "search/SearchService.hpp"

using nlohmann::json;
using namespace std::chrono_literals;

namespace {

constexpr unsigned short kPort = 18089;
const std::string kBaseUrl = "http://127.0.0.1:" + std::to_string(kPort);

enum class MockMode { Up, Down, Garbage, Slow };
std::atomic<MockMode> gMockMode{MockMode::Up};
std::atomic<int> gGenerateCalls{0};

std::filesystem::path gDatasetDir;
std::filesystem::path gFrontendDir;
constexpr int kDocuments = 120;

trantor::EventLoopThread& clientLoop() {
    static trantor::EventLoopThread loop("test-client");
    static std::once_flag once;
    std::call_once(once, [] { loop.run(); });
    return loop;
}

struct HttpResult {
    int status = 0;
    json body;
    std::string raw;
    std::string contentType;
};

HttpResult http(drogon::HttpMethod method, const std::string& path, const std::optional<std::string>& body = {},
                double timeout = 30.0) {
    auto client = drogon::HttpClient::newHttpClient(kBaseUrl, clientLoop().getLoop());
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(method);
    request->setPath(path);
    if (body) {
        request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        request->setBody(*body);
    }
    const auto [result, response] = client->sendRequest(request, timeout);
    HttpResult out;
    if (result != drogon::ReqResult::Ok || !response) {
        return out;
    }
    out.status = static_cast<int>(response->getStatusCode());
    out.raw = std::string(response->body());
    out.contentType = std::string(response->getHeader("content-type"));
    out.body = json::parse(out.raw, nullptr, false);
    return out;
}

HttpResult post(const std::string& path, const json& body) {
    return http(drogon::Post, path, body.dump());
}

std::string docName(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "document_%06d", i);
    return buf;
}

void writeDataset() {
    std::random_device rd;
    const auto root = std::filesystem::temp_directory_path() / ("search_it_" + std::to_string(rd()));
    gDatasetDir = root / "dataset";
    gFrontendDir = root / "frontend";
    std::filesystem::create_directories(gDatasetDir / "documents");
    std::filesystem::create_directories(gFrontendDir);
    std::ofstream(gFrontendDir / "index.html") << "<!doctype html><title>Search UI</title>";

    const char* topics[] = {
        "The car engine and the vehicle chassis were tested on the road.",
        "An automobile factory produced a new electric vehicle model.",
        "Machine learning models and neural networks learn from data.",
        "Artificial intelligence research improves machine reasoning.",
        "Cell biology studies protein folding and genome expression.",
        "Astronomy observes galaxies, stars and distant planets.",
    };
    for (int i = 1; i <= kDocuments; ++i) {
        std::ofstream out(gDatasetDir / "documents" / (docName(i) + ".txt"));
        out << "Document " << i << ". ";
        for (int k = 0; k <= i % 4; ++k) out << topics[i % 6] << ' ';
        out << "Common filler text about research and progress.\n";
    }
}

void registerMockOllama() {
    auto& app = drogon::app();
    app.registerHandler(
        "/mock-ollama/api/tags",
        [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            if (gMockMode == MockMode::Down) {
                resp->setStatusCode(drogon::k503ServiceUnavailable);
            } else {
                resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                resp->setBody(R"({"models":[{"name":"qwen2.5:3b","model":"qwen2.5:3b"}]})");
            }
            cb(resp);
        },
        {drogon::Get});
    app.registerHandler(
        "/mock-ollama/api/generate",
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            ++gGenerateCalls;
            const json request = json::parse(req->body(), nullptr, false);
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            switch (gMockMode.load()) {
                case MockMode::Down:
                    resp->setStatusCode(drogon::k500InternalServerError);
                    resp->setBody(R"({"error":"model crashed"})");
                    cb(resp);
                    return;
                case MockMode::Garbage:
                    resp->setBody(json{{"response", "Sorry, I can't produce JSON."}, {"done", true}}.dump());
                    cb(resp);
                    return;
                case MockMode::Slow:
                    // Reply after 3 s without blocking the event loop.
                    drogon::app().getLoop()->runAfter(3.0, [cb = std::move(cb), resp] {
                        resp->setBody(R"({"response":"{\"expanded_terms\":[\"late\"]}"})");
                        cb(resp);
                    });
                    return;
                case MockMode::Up: {
                    const bool valid = !request.is_discarded() && request.value("stream", true) == false &&
                                       request.value("format", "") == "json";
                    const json answer = {{"original", "car"},
                                         {"expanded_terms", valid ? json::array({"automobile", "vehicle"})
                                                                  : json::array({"invalid_request"})}};
                    resp->setBody(json{{"model", "qwen2.5:3b"}, {"response", answer.dump()}, {"done", true}}.dump());
                    cb(resp);
                    return;
                }
            }
        },
        {drogon::Post});
}

/// Collects WebSocket frames of one search.
struct WsSession {
    drogon::WebSocketClientPtr client;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<json> messages;
    bool connected = false;
    bool failed = false;

    bool connect() {
        client = drogon::WebSocketClient::newWebSocketClient("ws://127.0.0.1:" + std::to_string(kPort), clientLoop().getLoop());
        client->setMessageHandler([this](const std::string& message, const drogon::WebSocketClientPtr&,
                                         const drogon::WebSocketMessageType& type) {
            if (type != drogon::WebSocketMessageType::Text) return;
            std::lock_guard lock(mutex);
            messages.push_back(json::parse(message, nullptr, false));
            cv.notify_all();
        });
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/ws/search");
        client->connectToServer(req, [this](drogon::ReqResult r, const drogon::HttpResponsePtr&,
                                            const drogon::WebSocketClientPtr&) {
            std::lock_guard lock(mutex);
            connected = r == drogon::ReqResult::Ok;
            failed = !connected;
            cv.notify_all();
        });
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 10s, [this] { return connected || failed; });
        return connected;
    }

    void send(const std::string& text) { client->getConnection()->send(text); }

    /// Waits until at least `count` messages arrived (or the timeout expires).
    std::vector<json> waitFor(std::size_t count, std::chrono::seconds timeout = 30s) {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, timeout, [&] { return messages.size() >= count; });
        return messages;
    }

    void close() {
        if (client) client->stop();
    }
};

}  // namespace

// ============================================================================ REST

TEST(ApiIntegration, HealthBeforeIndexing) {
    gMockMode = MockMode::Up;
    const auto r = http(drogon::Get, "/api/health");
    ASSERT_EQ(r.status, 200);
    EXPECT_NE(r.contentType.find("application/json"), std::string::npos);
    EXPECT_EQ(r.body["status"], "ok");
    EXPECT_EQ(r.body["ollama"], true);
    EXPECT_EQ(r.body["index_ready"], false);
    EXPECT_EQ(r.body["model"], "qwen2.5:3b");
}

TEST(ApiIntegration, HealthReportsOllamaDown) {
    gMockMode = MockMode::Down;
    const auto r = http(drogon::Get, "/api/health");
    gMockMode = MockMode::Up;
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.body["status"], "ok");
    EXPECT_EQ(r.body["ollama"], false);
}

TEST(ApiIntegration, SearchWithoutIndexReturns503) {
    const auto r = post("/api/search", {{"query", "car"}, {"mode", "keyword"}});
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(r.body["error"], "index_not_ready");
    EXPECT_TRUE(r.body["message"].is_string());
}

TEST(ApiIntegration, StatsBeforeIndexing) {
    const auto r = http(drogon::Get, "/api/stats");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.body["documents"], 0);
    EXPECT_EQ(r.body["threads_used"], 0);
    EXPECT_EQ(r.body["index_ready"], false);
}

TEST(ApiIntegration, WebSocketWithoutIndexReturnsError) {
    WsSession ws;
    ASSERT_TRUE(ws.connect());
    ws.send(R"({"query":"car"})");
    const auto messages = ws.waitFor(1);
    ws.close();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0]["stage"], "error");
    EXPECT_EQ(messages[0]["data"]["error"], "index_not_ready");
}

TEST(ApiIntegration, IndexValidation) {
    for (const auto& body : {json{{"threads", 0}}, json{{"threads", -3}}, json{{"threads", "4"}},
                             json{{"threads", 2.5}}, json{{"threads", 100000}}, json{{"threads", nullptr}}}) {
        const auto r = post("/api/index", body);
        EXPECT_EQ(r.status, 400) << body.dump();
        EXPECT_EQ(r.body["error"], "invalid_threads") << body.dump();
    }
    const auto bad = http(drogon::Post, "/api/index", std::string("{threads: 4"));
    EXPECT_EQ(bad.status, 400);
    EXPECT_EQ(bad.body["error"], "invalid_json");
    const auto array = http(drogon::Post, "/api/index", std::string("[1,2]"));
    EXPECT_EQ(array.status, 400);
}

TEST(ApiIntegration, BuildIndex) {
    const auto r = post("/api/index", {{"threads", 4}});
    ASSERT_EQ(r.status, 200) << r.raw;
    EXPECT_EQ(r.body["status"], "ok");
    EXPECT_EQ(r.body["documents_indexed"], kDocuments);
    EXPECT_TRUE(r.body["time_ms"].is_number());
    EXPECT_GE(r.body["time_ms"].get<double>(), 0.0);
    EXPECT_EQ(r.body["threads_used"], 4);
}

TEST(ApiIntegration, StatsAfterIndexing) {
    const auto r = http(drogon::Get, "/api/stats");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.body["documents"], kDocuments);
    EXPECT_EQ(r.body["threads_used"], 4);
    EXPECT_TRUE(r.body["index_build_time_ms"].is_number());
    EXPECT_GT(r.body["unique_terms"].get<int>(), 10);
    EXPECT_GT(r.body["total_postings"].get<int>(), kDocuments);
    EXPECT_EQ(r.body["index_ready"], true);
    EXPECT_EQ(r.body["indexing_in_progress"], false);
}

TEST(ApiIntegration, IndexWithoutBodyUsesDefaultThreads) {
    const auto r = http(drogon::Post, "/api/index");
    ASSERT_EQ(r.status, 200) << r.raw;
    EXPECT_EQ(r.body["documents_indexed"], kDocuments);
    EXPECT_EQ(r.body["threads_used"], 2);  // INDEX_THREADS of the test config
}

TEST(ApiIntegration, SearchValidation) {
    struct Case {
        std::string body;
        std::string code;
    };
    const std::vector<Case> cases = {
        {"not json", "invalid_json"},
        {"[]", "invalid_json"},
        {R"({})", "invalid_query"},
        {R"({"query": ""})", "invalid_query"},
        {R"({"query": "   "})", "invalid_query"},
        {R"({"query": 42})", "invalid_query"},
        {R"({"query": "car", "mode": "semantic"})", "invalid_mode"},
        {R"({"query": "car", "mode": 1})", "invalid_mode"},
        {R"({"query": "car", "limit": 0})", "invalid_limit"},
        {R"({"query": "car", "limit": 1000})", "invalid_limit"},
        {R"({"query": "car", "limit": "5"})", "invalid_limit"},
        {R"({"query": "car", "expand": "yes"})", "invalid_expand"},
        {R"({"query": ")" + std::string(1001, 'a') + R"("})", "invalid_query"},
    };
    for (const auto& c : cases) {
        const auto r = http(drogon::Post, "/api/search", c.body);
        EXPECT_EQ(r.status, 400) << c.body.substr(0, 60);
        EXPECT_EQ(r.body["error"], c.code) << c.body.substr(0, 60);
        EXPECT_TRUE(r.body["message"].is_string());
    }
}

TEST(ApiIntegration, SearchWithQueryExpansion) {
    gMockMode = MockMode::Up;
    const auto r = post("/api/search", {{"query", "car"}, {"mode", "keyword"}, {"limit", 5}});
    ASSERT_EQ(r.status, 200) << r.raw;
    EXPECT_EQ(r.body["expansion_status"], "ok");
    EXPECT_EQ(r.body["expanded_terms"], json::array({"automobile", "vehicle"}));
    EXPECT_TRUE(r.body["fallback_reason"].is_null());
    EXPECT_EQ(r.body["query"], "car");
    EXPECT_TRUE(r.body["time_ms"].is_number());
    EXPECT_TRUE(r.body["search_time_ms"].is_number());
    EXPECT_TRUE(r.body["expansion_time_ms"].is_number());

    const auto& results = r.body["results"];
    ASSERT_EQ(results.size(), 5u);
    // car docs (i%6==0) and automobile docs (i%6==1) both match thanks to expansion
    EXPECT_EQ(r.body["total_matches"], 40);
    double previous = 1e18;
    for (const auto& item : results) {
        ASSERT_TRUE(item["doc_id"].is_string());
        ASSERT_TRUE(item["score"].is_number());
        ASSERT_TRUE(item["snippet"].is_string());
        EXPECT_EQ(item["doc_id"].get<std::string>().rfind("document_", 0), 0u);
        EXPECT_LE(item["score"].get<double>(), previous);
        previous = item["score"].get<double>();
        EXPECT_FALSE(item["snippet"].get<std::string>().empty());
    }
}

TEST(ApiIntegration, SearchFallbackWhenOllamaDown) {
    gMockMode = MockMode::Down;
    const auto r = post("/api/search", {{"query", "car"}, {"mode", "keyword"}});
    gMockMode = MockMode::Up;
    ASSERT_EQ(r.status, 200) << r.raw;
    EXPECT_EQ(r.body["expansion_status"], "fallback");
    EXPECT_TRUE(r.body["fallback_reason"].is_string());
    EXPECT_TRUE(r.body["expanded_terms"].empty());
    EXPECT_EQ(r.body["total_matches"], 20);  // only documents containing "car"
}

TEST(ApiIntegration, SearchFallbackOnInvalidLlmAnswer) {
    gMockMode = MockMode::Garbage;
    const auto r = post("/api/search", {{"query", "car"}});
    gMockMode = MockMode::Up;
    ASSERT_EQ(r.status, 200) << r.raw;
    EXPECT_EQ(r.body["expansion_status"], "fallback");
    EXPECT_EQ(r.body["total_matches"], 20);
}

TEST(ApiIntegration, SearchFallbackOnLlmTimeout) {
    gMockMode = MockMode::Slow;  // mock answers after 3 s, client timeout is 1 s
    const auto start = std::chrono::steady_clock::now();
    const auto r = post("/api/search", {{"query", "car"}});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    gMockMode = MockMode::Up;
    ASSERT_EQ(r.status, 200) << r.raw;
    EXPECT_EQ(r.body["expansion_status"], "fallback");
    EXPECT_LT(elapsed, 2900ms);
    std::this_thread::sleep_for(2500ms);  // let the slow mock reply drain
}

TEST(ApiIntegration, SearchWithExpansionDisabled) {
    const int callsBefore = gGenerateCalls.load();
    const auto r = post("/api/search", {{"query", "neural networks"}, {"expand", false}, {"limit", 3}});
    ASSERT_EQ(r.status, 200) << r.raw;
    EXPECT_EQ(r.body["expansion_status"], "disabled");
    EXPECT_EQ(gGenerateCalls.load(), callsBefore);
    EXPECT_EQ(r.body["results"].size(), 3u);
}

TEST(ApiIntegration, SearchWithoutModeDefaultsToKeyword) {
    const auto r = post("/api/search", {{"query", "galaxies"}, {"expand", false}});
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.body["total_matches"], 20);
}

TEST(ApiIntegration, SearchStopWordsOnlyReturnsEmpty) {
    const auto r = post("/api/search", {{"query", "the and of"}, {"expand", false}});
    ASSERT_EQ(r.status, 200);
    EXPECT_TRUE(r.body["results"].empty());
    EXPECT_EQ(r.body["total_matches"], 0);
}

TEST(ApiIntegration, ConcurrentSearchRequests) {
    constexpr int kClients = 24;
    constexpr int kRequestsPerClient = 10;
    std::atomic<int> ok{0};
    std::atomic<int> wrong{0};
    std::vector<std::thread> clients;
    for (int c = 0; c < kClients; ++c) {
        clients.emplace_back([&, c] {
            const char* queries[] = {"car", "protein genome", "machine learning", "galaxies stars"};
            for (int i = 0; i < kRequestsPerClient; ++i) {
                const auto r = post("/api/search", {{"query", queries[(c + i) % 4]}, {"expand", c % 3 == 0}});
                if (r.status == 200 && r.body["results"].is_array() && !r.body["results"].empty()) {
                    ++ok;
                } else {
                    ++wrong;
                }
            }
        });
    }
    for (auto& t : clients) t.join();
    EXPECT_EQ(ok.load(), kClients * kRequestsPerClient);
    EXPECT_EQ(wrong.load(), 0);
}

TEST(ApiIntegration, ConcurrentIndexRequestsAreSerialized) {
    std::vector<std::future<HttpResult>> futures;
    for (int i = 0; i < 4; ++i) {
        futures.push_back(std::async(std::launch::async, [] { return post("/api/index", {{"threads", 2}}); }));
    }
    int ok = 0;
    int conflict = 0;
    for (auto& f : futures) {
        const auto r = f.get();
        if (r.status == 200) ++ok;
        if (r.status == 409) {
            ++conflict;
            EXPECT_EQ(r.body["error"], "indexing_in_progress");
        }
    }
    EXPECT_GE(ok, 1);
    EXPECT_EQ(ok + conflict, 4);
    // the index is still usable afterwards
    EXPECT_EQ(post("/api/search", {{"query", "car"}, {"expand", false}}).status, 200);
}

TEST(ApiIntegration, UnknownRouteAndWrongMethod) {
    const auto missing = http(drogon::Get, "/api/does-not-exist");
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(missing.body["error"], "not_found");
    const auto wrongMethod = http(drogon::Get, "/api/search");
    EXPECT_EQ(wrongMethod.status, 405);
}

TEST(ApiIntegration, ServesWebUi) {
    const auto r = http(drogon::Get, "/");
    EXPECT_EQ(r.status, 200);
    EXPECT_NE(r.raw.find("Search UI"), std::string::npos);
}

// ============================================================================ WebSocket

TEST(ApiIntegration, WebSocketStreamsThreeStagesInOrder) {
    gMockMode = MockMode::Up;
    WsSession ws;
    ASSERT_TRUE(ws.connect());
    ws.send(R"({"query":"car"})");
    const auto messages = ws.waitFor(3);
    ws.close();
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(messages[0]["stage"], "query_expansion");
    EXPECT_EQ(messages[0]["data"]["original"], "car");
    EXPECT_EQ(messages[0]["data"]["expanded_terms"], json::array({"automobile", "vehicle"}));
    EXPECT_EQ(messages[0]["data"]["status"], "ok");

    EXPECT_EQ(messages[1]["stage"], "keyword_search");
    ASSERT_TRUE(messages[1]["data"]["results"].is_array());
    EXPECT_EQ(messages[1]["data"]["results"].size(), 10u);
    EXPECT_TRUE(messages[1]["data"]["time_ms"].is_number());
    EXPECT_EQ(messages[1]["data"]["total_matches"], 40);
    const auto& first = messages[1]["data"]["results"][0];
    EXPECT_TRUE(first["doc_id"].is_string());
    EXPECT_TRUE(first["score"].is_number());
    EXPECT_TRUE(first["snippet"].is_string());

    EXPECT_EQ(messages[2], json({{"stage", "done"}}));
}

TEST(ApiIntegration, WebSocketFallbackStage) {
    gMockMode = MockMode::Down;
    WsSession ws;
    ASSERT_TRUE(ws.connect());
    ws.send(R"({"query":"car","limit":2})");
    const auto messages = ws.waitFor(3);
    ws.close();
    gMockMode = MockMode::Up;
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(messages[0]["data"]["status"], "fallback");
    EXPECT_TRUE(messages[0]["data"]["fallback_reason"].is_string());
    EXPECT_EQ(messages[1]["data"]["results"].size(), 2u);
    EXPECT_EQ(messages[2]["stage"], "done");
}

TEST(ApiIntegration, WebSocketInvalidMessages) {
    WsSession ws;
    ASSERT_TRUE(ws.connect());
    ws.send("this is not json");
    auto messages = ws.waitFor(1);
    ASSERT_GE(messages.size(), 1u);
    EXPECT_EQ(messages[0]["stage"], "error");
    EXPECT_EQ(messages[0]["data"]["error"], "invalid_json");

    ws.send(R"({"query":""})");
    messages = ws.waitFor(2);
    ASSERT_GE(messages.size(), 2u);
    EXPECT_EQ(messages[1]["data"]["error"], "invalid_query");

    ws.send(R"({"text":"car"})");
    messages = ws.waitFor(3);
    ASSERT_GE(messages.size(), 3u);
    EXPECT_EQ(messages[2]["data"]["error"], "invalid_query");

    // the connection is still usable after errors
    ws.send(R"({"query":"protein","expand":false})");
    messages = ws.waitFor(6);
    ws.close();
    ASSERT_EQ(messages.size(), 6u);
    EXPECT_EQ(messages[3]["stage"], "query_expansion");
    EXPECT_EQ(messages[3]["data"]["status"], "disabled");
    EXPECT_EQ(messages[4]["stage"], "keyword_search");
    EXPECT_EQ(messages[5]["stage"], "done");
}

TEST(ApiIntegration, WebSocketSecondSearchWhileBusyIsRejected) {
    gMockMode = MockMode::Slow;
    WsSession ws;
    ASSERT_TRUE(ws.connect());
    ws.send(R"({"query":"car"})");
    std::this_thread::sleep_for(200ms);
    ws.send(R"({"query":"protein"})");
    const auto first = ws.waitFor(1, 5s);
    ASSERT_GE(first.size(), 1u);
    EXPECT_EQ(first[0]["stage"], "error");
    EXPECT_EQ(first[0]["data"]["error"], "search_in_progress");
    const auto all = ws.waitFor(4, 10s);  // the first search still completes (fallback after timeout)
    ws.close();
    gMockMode = MockMode::Up;
    ASSERT_EQ(all.size(), 4u);
    EXPECT_EQ(all[1]["stage"], "query_expansion");
    EXPECT_EQ(all[2]["stage"], "keyword_search");
    EXPECT_EQ(all[3]["stage"], "done");
    std::this_thread::sleep_for(2500ms);
}

TEST(ApiIntegration, ManyWebSocketClientsAreIndependent) {
    gMockMode = MockMode::Up;
    constexpr int kClients = 12;
    std::vector<std::unique_ptr<WsSession>> sessions;
    for (int i = 0; i < kClients; ++i) {
        sessions.push_back(std::make_unique<WsSession>());
        ASSERT_TRUE(sessions.back()->connect());
    }
    const char* queries[] = {"car", "protein", "galaxies", "machine"};
    for (int i = 0; i < kClients; ++i) {
        sessions[i]->send(json{{"query", queries[i % 4]}, {"limit", 3}, {"expand", i % 2 == 0}}.dump());
    }
    for (int i = 0; i < kClients; ++i) {
        const auto messages = sessions[i]->waitFor(3);
        ASSERT_EQ(messages.size(), 3u) << "client " << i;
        EXPECT_EQ(messages[0]["stage"], "query_expansion");
        EXPECT_EQ(messages[0]["data"]["original"], queries[i % 4]);
        EXPECT_EQ(messages[1]["stage"], "keyword_search");
        EXPECT_EQ(messages[1]["data"]["results"].size(), 3u);
        EXPECT_EQ(messages[2]["stage"], "done");
        sessions[i]->close();
    }
}

// ============================================================================ Ollama client

TEST(OllamaClientIntegration, UnreachableServerFailsFast) {
    search::ollama::OllamaClient client("http://127.0.0.1:1", "qwen2.5:3b", 5.0, 1.0, 0.0);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(client.isAvailable());
    std::string error;
    EXPECT_FALSE(client.generate("hello", &error).has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
}

TEST(OllamaClientIntegration, TalksToMockServer) {
    gMockMode = MockMode::Up;
    search::ollama::OllamaClient client(kBaseUrl + "/mock-ollama/", "qwen2.5:3b", 5.0, 1.0, 0.0);
    EXPECT_TRUE(client.isAvailable());
    std::string error;
    const auto text = client.generate("query: car", &error);
    ASSERT_TRUE(text.has_value()) << error;
    EXPECT_NE(text->find("automobile"), std::string::npos);
    search::ollama::OllamaClient wrongModel(kBaseUrl + "/mock-ollama", "llama3:70b", 5.0, 1.0, 0.0);
    EXPECT_FALSE(wrongModel.isAvailable());
}

// ============================================================================ main

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    writeDataset();

    search::core::Config config;
    config.datasetPath = gDatasetDir.string();
    config.frontendPath = gFrontendDir.string();
    config.requestThreads = 8;
    config.indexThreads = 2;
    config.autoIndex = false;
    config.ollamaUrl = kBaseUrl + "/mock-ollama";
    config.ollamaTimeoutSec = 1.0;
    config.ollamaHealthTimeoutSec = 1.0;
    config.ollamaHealthTtlSec = 0.0;  // no caching: tests switch the mock mode

    auto ollama = std::make_shared<search::ollama::OllamaClient>(config.ollamaUrl, config.ollamaModel,
                                                                 config.ollamaTimeoutSec,
                                                                 config.ollamaHealthTimeoutSec,
                                                                 config.ollamaHealthTtlSec);
    auto service = std::make_shared<search::engine::SearchService>(config, ollama);
    search::api::registerRoutes(service);
    registerMockOllama();

    trantor::Logger::setLogLevel(trantor::Logger::kWarn);
    drogon::app().setDocumentRoot(config.frontendPath).addListener("127.0.0.1", kPort).setThreadNum(4).setLogLevel(
        trantor::Logger::kWarn);

    std::thread server([] { drogon::app().run(); });
    // wait until the server answers
    bool up = false;
    for (int i = 0; i < 100 && !up; ++i) {
        std::this_thread::sleep_for(100ms);
        up = http(drogon::Get, "/api/stats", std::nullopt, 1.0).status == 200;
    }
    int rc = 1;
    if (up) {
        rc = RUN_ALL_TESTS();
    } else {
        std::cerr << "server did not start\n";
    }

    drogon::app().quit();
    server.join();
    service->shutdown();
    std::error_code ec;
    std::filesystem::remove_all(gDatasetDir.parent_path(), ec);
    return rc;
}
