#include "core/Config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>

namespace search::core {
namespace {

const char* rawEnv(const char* name) {
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? value : nullptr;
}

void readString(const char* name, std::string& target) {
    if (const char* value = rawEnv(name)) {
        target = value;
    }
}

/// Reads an integer in [minValue, maxValue]. Malformed, negative or out-of-range
/// values keep the default and are reported in `warnings`.
template <typename T>
void readUnsigned(const char* name, T& target, long long minValue, long long maxValue,
                  std::vector<std::string>& warnings) {
    const char* value = rawEnv(name);
    if (value == nullptr) {
        return;
    }
    try {
        std::size_t used = 0;
        const long long parsed = std::stoll(value, &used);
        if (used != std::char_traits<char>::length(value) || parsed < minValue || parsed > maxValue) {
            throw std::out_of_range("range");
        }
        target = static_cast<T>(parsed);
    } catch (const std::exception&) {
        warnings.push_back(std::string(name) + "=" + value + " ignored (expected an integer in [" +
                           std::to_string(minValue) + ", " + std::to_string(maxValue) + "]), using " +
                           std::to_string(target));
    }
}

void readDouble(const char* name, double& target, std::vector<std::string>& warnings) {
    const char* value = rawEnv(name);
    if (value == nullptr) {
        return;
    }
    try {
        std::size_t used = 0;
        const double parsed = std::stod(value, &used);
        if (used != std::char_traits<char>::length(value) || parsed < 0.0 || parsed > 86400.0) {
            throw std::out_of_range("range");
        }
        target = parsed;
    } catch (const std::exception&) {
        warnings.push_back(std::string(name) + "=" + value + " ignored (expected a non-negative number)");
    }
}

void readBool(const char* name, bool& target) {
    if (const char* value = rawEnv(name)) {
        std::string lower(value);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        target = (lower == "1" || lower == "true" || lower == "yes" || lower == "on");
    }
}

}  // namespace

Config Config::fromEnvironment() {
    Config cfg;
    const auto hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    cfg.indexThreads = hw;
    auto& w = cfg.warnings;
    constexpr long long kMaxThreads = 1024;

    readString("DATASET_PATH", cfg.datasetPath);
    readString("LISTEN_ADDRESS", cfg.listenAddress);
    readUnsigned("SERVER_PORT", cfg.port, 1, 65535, w);
    readUnsigned("IO_THREADS", cfg.ioThreads, 1, kMaxThreads, w);
    readUnsigned("REQUEST_THREADS", cfg.requestThreads, 1, kMaxThreads, w);
    readUnsigned("MAX_PENDING_REQUESTS", cfg.maxPendingRequests, 1, 1'000'000, w);
    readString("FRONTEND_PATH", cfg.frontendPath);

    readBool("AUTO_INDEX", cfg.autoIndex);
    readUnsigned("MAX_INDEX_THREADS", cfg.maxIndexThreads, 1, kMaxThreads, w);
    readUnsigned("INDEX_THREADS", cfg.indexThreads, 1, kMaxThreads, w);

    readUnsigned("MAX_LIMIT", cfg.maxLimit, 1, 10'000, w);
    readUnsigned("DEFAULT_LIMIT", cfg.defaultLimit, 1, 10'000, w);
    readUnsigned("MAX_QUERY_LENGTH", cfg.maxQueryLength, 1, 60'000, w);
    readDouble("EXPANDED_TERM_WEIGHT", cfg.expandedTermWeight, w);

    readString("OLLAMA_URL", cfg.ollamaUrl);
    readString("OLLAMA_MODEL", cfg.ollamaModel);
    readDouble("OLLAMA_TIMEOUT_SEC", cfg.ollamaTimeoutSec, w);
    readDouble("OLLAMA_HEALTH_TIMEOUT_SEC", cfg.ollamaHealthTimeoutSec, w);
    readDouble("OLLAMA_HEALTH_TTL_SEC", cfg.ollamaHealthTtlSec, w);
    readUnsigned("MAX_EXPANSION_TERMS", cfg.maxExpansionTerms, 0, 50, w);
    readUnsigned("LLM_THREADS", cfg.llmThreads, 1, 256, w);
    readUnsigned("LLM_MAX_QUEUE", cfg.llmMaxQueue, 1, 100'000, w);

    readString("LOG_LEVEL", cfg.logLevel);

    cfg.defaultLimit = std::min(cfg.defaultLimit, cfg.maxLimit);
    cfg.indexThreads = std::min(cfg.indexThreads, cfg.maxIndexThreads);
    return cfg;
}

}  // namespace search::core
