#pragma once

#include <optional>
#include <string>

namespace search::ollama {

/// Pure (I/O-free) helpers for the Ollama HTTP API, kept separate from the
/// network client so they can be unit-tested.
struct OllamaProtocol {
    /// JSON body for POST /api/generate (non-streaming, JSON output mode).
    static std::string buildGenerateRequest(const std::string& model, const std::string& prompt,
                                            int maxTokens = 128, double temperature = 0.2,
                                            const std::string& keepAlive = "30m");

    /// Extracts the "response" field of a /api/generate reply. std::nullopt if the
    /// body is not valid JSON, contains "error" or has no string "response".
    static std::optional<std::string> extractResponseText(const std::string& body, std::string* error = nullptr);

    /// True if the /api/tags body lists `model` (accepts "name" and "model" keys and
    /// treats "qwen2.5:3b" and "qwen2.5:3b-..." / ":latest" variants sensibly).
    static bool modelListed(const std::string& tagsBody, const std::string& model);
};

}  // namespace search::ollama
