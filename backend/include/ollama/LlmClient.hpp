#pragma once

#include <optional>
#include <string>

namespace search::ollama {

/// Abstract text-generation client. Implemented by OllamaClient (HTTP) and by
/// fakes in unit tests, so query expansion logic is testable without a model.
class LlmClient {
public:
    virtual ~LlmClient() = default;

    /// True if the LLM backend is reachable and the model is available.
    /// Implementations may cache the answer for a short time.
    virtual bool isAvailable() = 0;

    /// Sends `prompt` and returns the generated text, or std::nullopt on any
    /// error (then `error`, if not null, receives a human-readable reason).
    /// Must never block longer than min(timeoutSec, configured timeout);
    /// timeoutSec <= 0 means "use the configured timeout".
    virtual std::optional<std::string> generate(const std::string& prompt, std::string* error,
                                                double timeoutSec = 0.0) = 0;

    /// Model name used for generation (for logs / health output).
    [[nodiscard]] virtual std::string model() const = 0;
};

}  // namespace search::ollama
