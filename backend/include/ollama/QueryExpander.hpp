#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ollama/LlmClient.hpp"

namespace search::ollama {

enum class ExpansionStatus {
    Ok,        ///< LLM answered and its terms were used
    Fallback,  ///< LLM unavailable / failed / invalid answer -> plain keyword search
    Disabled,  ///< expansion switched off by the client (expand=false) or by config
};

std::string toString(ExpansionStatus status);

struct ExpansionResult {
    std::string original;
    std::vector<std::string> expandedTerms;  ///< sanitized phrases returned by the LLM
    ExpansionStatus status = ExpansionStatus::Disabled;
    std::string fallbackReason;              ///< empty unless status == Fallback
    double timeMs = 0.0;
};

/// LLM-based query expansion: asks the model for synonyms/related keywords in a
/// strict JSON format and defensively parses the answer. The LLM never performs
/// the search itself; it only contributes additional keywords.
class QueryExpander {
public:
    QueryExpander(std::shared_ptr<LlmClient> client, std::size_t maxTerms);

    /// Never throws: every failure results in status == Fallback with a reason.
    ExpansionResult expand(const std::string& query) const;

    /// Structured prompt requiring a JSON object {"original": ..., "expanded_terms": [...]}.
    static std::string buildPrompt(const std::string& query, std::size_t maxTerms);

    /// Parses the model output. Accepts a JSON object (optionally surrounded by text
    /// or a ```json fence) with "expanded_terms" (or a bare JSON array). Terms are
    /// sanitized, de-duplicated, terms equal to the original query are removed and
    /// the list is truncated to maxTerms. Returns std::nullopt if no valid JSON.
    static std::optional<std::vector<std::string>> parseResponse(const std::string& llmText,
                                                                 const std::string& originalQuery,
                                                                 std::size_t maxTerms);

    /// Lowercases, keeps [a-z0-9 -], collapses spaces, trims; empty if invalid or too long.
    static std::string sanitizeTerm(const std::string& term);

    static constexpr std::size_t kMaxTermLength = 40;

private:
    std::shared_ptr<LlmClient> client_;
    std::size_t maxTerms_;
};

}  // namespace search::ollama
