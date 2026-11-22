// Command-line client for the search backend.
//
//   search "artificial intelligence"
//   search "artificial intelligence" --limit 10
//   search "machine learning" --server http://localhost:8080
//   search "car" --no-expand --json

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <drogon/HttpClient.h>
#include <nlohmann/json.hpp>
#include <trantor/net/EventLoopThread.h>
#include <trantor/utils/Logger.h>

using nlohmann::json;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitConnection = 3;
constexpr int kExitServerError = 4;

struct Options {
    std::string query;
    std::string server = "http://localhost:8080";
    long limit = 10;
    bool expand = true;
    bool rawJson = false;
    double timeoutSec = 120.0;
};

void printUsage(std::ostream& out) {
    out << "Usage: search \"<query>\" [options]\n\n"
           "Options:\n"
           "  --limit N        number of results (1-100, default 10)\n"
           "  --server URL     backend URL (default $SEARCH_SERVER or http://localhost:8080)\n"
           "  --no-expand      disable Ollama query expansion\n"
           "  --timeout SEC    request timeout in seconds (default 120)\n"
           "  --json           print the raw JSON response\n"
           "  -h, --help       show this help\n\n"
           "Examples:\n"
           "  search \"artificial intelligence\"\n"
           "  search \"artificial intelligence\" --limit 10\n"
           "  search \"machine learning\" --server http://localhost:8080\n";
}

bool parseArgs(int argc, char** argv, Options& options) {
    if (const char* env = std::getenv("SEARCH_SERVER"); env != nullptr && *env != '\0') {
        options.server = env;
    }
    std::vector<std::string> words;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "error: " << name << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            printUsage(std::cout);
            std::exit(kExitOk);
        } else if (arg == "--limit") {
            const char* v = needValue("--limit");
            if (v == nullptr) return false;
            char* end = nullptr;
            options.limit = std::strtol(v, &end, 10);
            if (*end != '\0' || options.limit < 1 || options.limit > 100) {
                std::cerr << "error: --limit must be an integer between 1 and 100\n";
                return false;
            }
        } else if (arg == "--server") {
            const char* v = needValue("--server");
            if (v == nullptr) return false;
            options.server = v;
        } else if (arg == "--timeout") {
            const char* v = needValue("--timeout");
            if (v == nullptr) return false;
            options.timeoutSec = std::atof(v);
            if (options.timeoutSec <= 0) {
                std::cerr << "error: --timeout must be positive\n";
                return false;
            }
        } else if (arg == "--no-expand") {
            options.expand = false;
        } else if (arg == "--json") {
            options.rawJson = true;
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "error: unknown option " << arg << "\n";
            return false;
        } else {
            words.push_back(arg);
        }
    }
    for (const auto& w : words) {
        if (!options.query.empty()) options.query += ' ';
        options.query += w;
    }
    if (options.query.empty()) {
        std::cerr << "error: query is required\n\n";
        printUsage(std::cerr);
        return false;
    }
    while (!options.server.empty() && options.server.back() == '/') {
        options.server.pop_back();
    }
    return true;
}

void printResults(const json& body) {
    const std::string status = body.value("expansion_status", "disabled");
    std::cout << "Query:    \"" << body.value("query", "") << "\"\n";
    if (status == "ok") {
        std::cout << "Expanded: ";
        const auto& terms = body["expanded_terms"];
        if (terms.empty()) std::cout << "(no additional terms)";
        for (std::size_t i = 0; i < terms.size(); ++i) {
            std::cout << (i ? ", " : "") << terms[i].get<std::string>();
        }
        std::cout << "  [Ollama, " << body.value("expansion_time_ms", 0.0) << " ms]\n";
    } else if (status == "fallback") {
        std::cout << "Expanded: Ollama unavailable - fallback to plain keyword search";
        if (body.contains("fallback_reason") && body["fallback_reason"].is_string()) {
            std::cout << " (" << body["fallback_reason"].get<std::string>() << ")";
        }
        std::cout << "\n";
    } else {
        std::cout << "Expanded: disabled\n";
    }

    const auto& results = body["results"];
    std::cout << "Found " << body.value("total_matches", 0) << " matching documents, showing " << results.size()
              << "\n";
    std::cout << "Search time: " << body.value("search_time_ms", 0.0) << " ms (index), "
              << body.value("time_ms", 0.0) << " ms (total)\n\n";

    int rank = 1;
    for (const auto& r : results) {
        std::cout << std::setw(3) << rank++ << ". " << r.value("doc_id", "") << "   score " << std::fixed
                  << std::setprecision(6) << r.value("score", 0.0) << std::defaultfloat << "\n";
        std::cout << "     " << r.value("snippet", "") << "\n\n";
    }
    if (results.empty()) {
        std::cout << "No documents matched the query.\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseArgs(argc, argv, options)) {
        return kExitUsage;
    }
    trantor::Logger::setLogLevel(trantor::Logger::kFatal);  // keep CLI output clean

    trantor::EventLoopThread loopThread("search-cli");
    loopThread.run();

    auto client = drogon::HttpClient::newHttpClient(options.server, loopThread.getLoop());
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setPath("/api/search");
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->setBody(json{{"query", options.query}, {"mode", "keyword"}, {"limit", options.limit},
                          {"expand", options.expand}}
                         .dump());

    const auto [result, response] = client->sendRequest(request, options.timeoutSec);
    if (result != drogon::ReqResult::Ok || !response) {
        std::cerr << "error: cannot reach server " << options.server << " ("
                  << (result == drogon::ReqResult::Timeout ? "timeout" : "connection failed") << ")\n";
        return kExitConnection;
    }

    const json body = json::parse(response->body(), nullptr, false);
    if (body.is_discarded()) {
        std::cerr << "error: server returned non-JSON response (HTTP " << static_cast<int>(response->getStatusCode())
                  << ")\n";
        return kExitServerError;
    }
    if (response->getStatusCode() != drogon::k200OK) {
        std::cerr << "error: HTTP " << static_cast<int>(response->getStatusCode()) << " "
                  << body.value("error", "unknown_error") << ": " << body.value("message", "") << "\n";
        return kExitServerError;
    }
    if (options.rawJson) {
        std::cout << body.dump(2) << "\n";
    } else {
        printResults(body);
    }
    return kExitOk;
}
