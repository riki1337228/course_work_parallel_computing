#include "ollama/QueryExpander.hpp"

#include <chrono>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "core/TextProcessor.hpp"

namespace search::ollama {

using nlohmann::json;

std::string toString(ExpansionStatus status) {
    switch (status) {
        case ExpansionStatus::Ok:
            return "ok";
        case ExpansionStatus::Fallback:
            return "fallback";
        case ExpansionStatus::Disabled:
            return "disabled";
    }
    return "unknown";
}

QueryExpander::QueryExpander(std::shared_ptr<LlmClient> client, std::size_t maxTerms)
    : client_(std::move(client)), maxTerms_(maxTerms) {}

std::string QueryExpander::buildPrompt(const std::string& query, std::size_t maxTerms) {
    // The query is embedded as a JSON string literal, which escapes quotes and
    // control characters and limits prompt-injection through the query text.
    const std::string quoted = json(query).dump();
    std::string prompt;
    prompt += "You are a query expansion component of an English keyword search engine.\n";
    prompt += "Task: for the user query below, list additional search keywords: synonyms, ";
    prompt += "closely related terms and common alternative spellings.\n";
    prompt += "Rules:\n";
    prompt += "- Answer with ONE JSON object and nothing else.\n";
    prompt += "- Format: {\"original\": <the query>, \"expanded_terms\": [<term>, ...]}\n";
    prompt += "- At most " + std::to_string(maxTerms) + " terms; each term is 1-2 lowercase English words.\n";
    prompt += "- Do not repeat words of the original query. No explanations.\n";
    prompt += "Example: query \"doctor\" -> {\"original\": \"doctor\", \"expanded_terms\": ";
    prompt += "[\"physician\", \"medic\", \"clinician\", \"surgeon\"]}\n";
    prompt += "User query: " + quoted + "\n";
    return prompt;
}

std::string QueryExpander::sanitizeTerm(const std::string& term) {
    std::string out;
    out.reserve(term.size());
    bool lastSpace = true;
    for (const char raw : term) {
        const auto c = static_cast<unsigned char>(raw);
        char ch = 0;
        if (c >= 'A' && c <= 'Z') {
            ch = static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
            ch = static_cast<char>(c);
        } else if (c == ' ' || c == '_' || c == '\t') {
            ch = ' ';
        } else {
            continue;  // drop punctuation, quotes, non-ASCII
        }
        if (ch == ' ') {
            if (!lastSpace) {
                out.push_back(' ');
            }
            lastSpace = true;
        } else {
            out.push_back(ch);
            lastSpace = false;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '-')) {
        out.pop_back();
    }
    while (!out.empty() && out.front() == '-') {
        out.erase(out.begin());
    }
    if (out.size() > kMaxTermLength) {
        return {};
    }
    return out;
}

std::optional<std::vector<std::string>> QueryExpander::parseResponse(const std::string& llmText,
                                                                     const std::string& originalQuery,
                                                                     std::size_t maxTerms) {
    json parsed = json::parse(llmText, nullptr, false);
    if (parsed.is_discarded()) {
        // Tolerate text/markdown around the JSON: take the outermost {...} block.
        const auto first = llmText.find('{');
        const auto last = llmText.rfind('}');
        if (first == std::string::npos || last == std::string::npos || last <= first) {
            return std::nullopt;
        }
        parsed = json::parse(llmText.substr(first, last - first + 1), nullptr, false);
        if (parsed.is_discarded()) {
            return std::nullopt;
        }
    }

    const json* list = nullptr;
    if (parsed.is_array()) {
        list = &parsed;
    } else if (parsed.is_object()) {
        for (const char* key : {"expanded_terms", "terms", "keywords", "synonyms"}) {
            const auto it = parsed.find(key);
            if (it != parsed.end() && it->is_array()) {
                list = &*it;
                break;
            }
        }
    }
    if (list == nullptr) {
        return std::nullopt;
    }

    const std::string originalNormalized = sanitizeTerm(originalQuery);
    std::unordered_set<std::string> originalTokens;
    for (auto& token : core::TextProcessor::uniqueTokens(originalQuery)) {
        originalTokens.insert(std::move(token));
    }

    std::vector<std::string> terms;
    std::unordered_set<std::string> seen;
    for (const auto& item : *list) {
        if (terms.size() >= maxTerms) {
            break;
        }
        if (!item.is_string()) {
            continue;  // ignore numbers, objects, nulls
        }
        std::string term = sanitizeTerm(item.get<std::string>());
        if (term.empty() || term == originalNormalized || seen.contains(term)) {
            continue;
        }
        // Skip terms that add nothing to the query (only stop words / original words).
        bool contributes = false;
        for (const auto& token : core::TextProcessor::uniqueTokens(term)) {
            if (!originalTokens.contains(token)) {
                contributes = true;
                break;
            }
        }
        if (!contributes) {
            continue;
        }
        seen.insert(term);
        terms.push_back(std::move(term));
    }
    return terms;
}

ExpansionResult QueryExpander::expand(const std::string& query, Clock::time_point deadline) const {
    const auto start = std::chrono::steady_clock::now();
    ExpansionResult result;
    result.original = query;
    auto finish = [&](ExpansionStatus status, std::string reason = {}) {
        result.status = status;
        result.fallbackReason = std::move(reason);
        result.timeMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return result;
    };

    if (!client_ || maxTerms_ == 0) {
        return finish(ExpansionStatus::Disabled);
    }
    if (core::TextProcessor::uniqueTokens(query).empty()) {
        // Nothing to expand (only stop words / punctuation): asking the LLM would
        // only produce unrelated terms, so the query is left as is.
        return finish(ExpansionStatus::Ok);
    }
    try {
        if (!client_->isAvailable()) {
            return finish(ExpansionStatus::Fallback, "Ollama is unavailable");
        }
        double budgetSec = 0.0;  // 0 = client's configured timeout
        if (deadline != Clock::time_point::max()) {
            const auto remaining = deadline - Clock::now();
            if (remaining < kMinLlmBudget) {
                return finish(ExpansionStatus::Fallback,
                              "LLM is busy: no free slot within the time budget");
            }
            budgetSec = std::chrono::duration<double>(remaining).count();
        }
        std::string error;
        const auto text = client_->generate(buildPrompt(query, maxTerms_), &error, budgetSec);
        if (!text) {
            return finish(ExpansionStatus::Fallback, error.empty() ? "Ollama request failed" : error);
        }
        auto terms = parseResponse(*text, query, maxTerms_);
        if (!terms) {
            return finish(ExpansionStatus::Fallback, "LLM returned an invalid (non-JSON) answer");
        }
        result.expandedTerms = std::move(*terms);
        return finish(ExpansionStatus::Ok);
    } catch (const std::exception& e) {
        return finish(ExpansionStatus::Fallback, std::string("query expansion error: ") + e.what());
    }
}

}  // namespace search::ollama
