#include "core/Config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
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

template <typename T>
void readUnsigned(const char* name, T& target, T minValue) {
    if (const char* value = rawEnv(name)) {
        try {
            const unsigned long long parsed = std::stoull(value);
            target = std::max(static_cast<T>(parsed), minValue);
        } catch (const std::exception&) {
            // keep default on malformed value
        }
    }
}

void readDouble(const char* name, double& target) {
    if (const char* value = rawEnv(name)) {
        try {
            const double parsed = std::stod(value);
            if (parsed >= 0.0) {
                target = parsed;
            }
        } catch (const std::exception&) {
        }
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

    readString("DATASET_PATH", cfg.datasetPath);
    readString("LISTEN_ADDRESS", cfg.listenAddress);
    readUnsigned<unsigned short>("SERVER_PORT", cfg.port, 1);
    readUnsigned<std::size_t>("IO_THREADS", cfg.ioThreads, 1);
    readUnsigned<std::size_t>("REQUEST_THREADS", cfg.requestThreads, 1);
    readString("FRONTEND_PATH", cfg.frontendPath);

    readBool("AUTO_INDEX", cfg.autoIndex);
    readUnsigned<std::size_t>("INDEX_THREADS", cfg.indexThreads, 1);
    readUnsigned<std::size_t>("MAX_INDEX_THREADS", cfg.maxIndexThreads, 1);

    readUnsigned<std::size_t>("DEFAULT_LIMIT", cfg.defaultLimit, 1);
    readUnsigned<std::size_t>("MAX_LIMIT", cfg.maxLimit, 1);
    readUnsigned<std::size_t>("MAX_QUERY_LENGTH", cfg.maxQueryLength, 1);
    readDouble("EXPANDED_TERM_WEIGHT", cfg.expandedTermWeight);

    readString("OLLAMA_URL", cfg.ollamaUrl);
    readString("OLLAMA_MODEL", cfg.ollamaModel);
    readDouble("OLLAMA_TIMEOUT_SEC", cfg.ollamaTimeoutSec);
    readDouble("OLLAMA_HEALTH_TIMEOUT_SEC", cfg.ollamaHealthTimeoutSec);
    readDouble("OLLAMA_HEALTH_TTL_SEC", cfg.ollamaHealthTtlSec);
    readUnsigned<std::size_t>("MAX_EXPANSION_TERMS", cfg.maxExpansionTerms, 0);

    readString("LOG_LEVEL", cfg.logLevel);

    cfg.defaultLimit = std::min(cfg.defaultLimit, cfg.maxLimit);
    cfg.indexThreads = std::min(cfg.indexThreads, cfg.maxIndexThreads);
    return cfg;
}

}  // namespace search::core
