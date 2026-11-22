#include "ollama/OllamaProtocol.hpp"

#include <nlohmann/json.hpp>

namespace search::ollama {

using nlohmann::json;

std::string OllamaProtocol::buildGenerateRequest(const std::string& model, const std::string& prompt, int maxTokens,
                                                 double temperature, const std::string& keepAlive) {
    const json body = {
        {"model", model},
        {"prompt", prompt},
        {"stream", false},
        {"format", "json"},
        {"keep_alive", keepAlive},
        {"options", {{"temperature", temperature}, {"num_predict", maxTokens}}},
    };
    return body.dump();
}

std::optional<std::string> OllamaProtocol::extractResponseText(const std::string& body, std::string* error) {
    auto fail = [error](const std::string& message) -> std::optional<std::string> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };
    const json parsed = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return fail("Ollama returned a non-JSON body");
    }
    if (const auto it = parsed.find("error"); it != parsed.end()) {
        return fail("Ollama error: " + (it->is_string() ? it->get<std::string>() : it->dump()));
    }
    const auto it = parsed.find("response");
    if (it == parsed.end() || !it->is_string()) {
        return fail("Ollama reply has no 'response' string");
    }
    return it->get<std::string>();
}

bool OllamaProtocol::modelListed(const std::string& tagsBody, const std::string& model) {
    const json parsed = json::parse(tagsBody, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return false;
    }
    const auto models = parsed.find("models");
    if (models == parsed.end() || !models->is_array()) {
        return false;
    }
    const bool requestHasTag = model.find(':') != std::string::npos;
    for (const auto& entry : *models) {
        if (!entry.is_object()) {
            continue;
        }
        for (const char* key : {"name", "model"}) {
            const auto field = entry.find(key);
            if (field == entry.end() || !field->is_string()) {
                continue;
            }
            const std::string name = field->get<std::string>();
            if (name == model || (!requestHasTag && name == model + ":latest")) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace search::ollama
