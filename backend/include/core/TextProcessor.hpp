#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace search::core {

/// Text normalization and tokenization shared by indexing and querying.
///
/// Pipeline: lowercase (ASCII) -> punctuation/non-alphanumeric bytes become
/// separators -> split into tokens -> drop tokens shorter than kMinTokenLength
/// or longer than kMaxTokenLength -> drop English stop words.
class TextProcessor {
public:
    static constexpr std::size_t kMinTokenLength = 2;
    static constexpr std::size_t kMaxTokenLength = 48;

    /// Lowercases ASCII letters and replaces every non-alphanumeric byte with ' '.
    static void normalizeInPlace(std::string& text) noexcept;

    /// Returns a normalized copy of `text`.
    static std::string normalize(std::string_view text);

    /// True if `token` (already lowercase) is an English stop word.
    static bool isStopWord(std::string_view token) noexcept;

    /// True if a normalized token passes length and stop-word filters.
    static bool isIndexable(std::string_view token) noexcept;

    /// Calls `sink(std::string_view)` for every indexable token of an
    /// already-normalized text. Views point into `normalized`; no allocations.
    template <typename Sink>
    static void forEachToken(std::string_view normalized, Sink&& sink) {
        std::size_t pos = 0;
        const std::size_t n = normalized.size();
        while (pos < n) {
            while (pos < n && normalized[pos] == ' ') {
                ++pos;
            }
            const std::size_t start = pos;
            while (pos < n && normalized[pos] != ' ') {
                ++pos;
            }
            if (pos > start) {
                const std::string_view token = normalized.substr(start, pos - start);
                if (isIndexable(token)) {
                    sink(token);
                }
            }
        }
    }

    /// Full pipeline on raw text: returns indexable tokens in order of appearance.
    static std::vector<std::string> tokenize(std::string_view text);

    /// Same as tokenize(), but keeps only the first occurrence of each token.
    static std::vector<std::string> uniqueTokens(std::string_view text);
};

}  // namespace search::core
