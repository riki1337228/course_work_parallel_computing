#include "core/TextProcessor.hpp"

#include <array>
#include <unordered_set>

namespace search::core {
namespace {

/// Lookup table: maps every byte to its normalized form (lowercase letter, digit or ' ').
constexpr std::array<char, 256> makeNormalizationTable() {
    std::array<char, 256> table{};
    for (int c = 0; c < 256; ++c) {
        char out = ' ';
        if (c >= 'a' && c <= 'z') {
            out = static_cast<char>(c);
        } else if (c >= 'A' && c <= 'Z') {
            out = static_cast<char>(c - 'A' + 'a');
        } else if (c >= '0' && c <= '9') {
            out = static_cast<char>(c);
        }
        table[static_cast<std::size_t>(c)] = out;
    }
    return table;
}

constexpr std::array<char, 256> kNormalizationTable = makeNormalizationTable();

const std::unordered_set<std::string_view>& stopWords() {
    static const std::unordered_set<std::string_view> kStopWords = {
        "a", "about", "above", "after", "again", "against", "all", "also", "am", "an", "and",
        "any", "are", "as", "at", "be", "because", "been", "before", "being", "below",
        "between", "both", "but", "by", "can", "could", "did", "do", "does", "doing", "down",
        "during", "each", "few", "for", "from", "further", "had", "has", "have", "having", "he",
        "her", "here", "hers", "herself", "him", "himself", "his", "how", "however", "if", "in",
        "into", "is", "it", "its", "itself", "just", "me", "more", "most", "my", "myself", "no",
        "nor", "not", "now", "of", "off", "on", "once", "only", "or", "other", "our", "ours",
        "ourselves", "out", "over", "own", "same", "she", "should", "so", "some", "such", "than",
        "that", "the", "their", "theirs", "them", "themselves", "then", "there", "these", "they",
        "this", "those", "through", "to", "too", "under", "until", "up", "very", "was", "we",
        "were", "what", "when", "where", "which", "while", "who", "whom", "why", "will", "with",
        "would", "you", "your", "yours", "yourself", "yourselves", "s", "t", "don", "doesn",
        "didn", "isn", "aren", "wasn", "weren", "won", "ll", "re", "ve", "d", "m", "o", "y",
    };
    return kStopWords;
}

}  // namespace

void TextProcessor::normalizeInPlace(std::string& text) noexcept {
    for (char& ch : text) {
        ch = kNormalizationTable[static_cast<unsigned char>(ch)];
    }
}

std::string TextProcessor::normalize(std::string_view text) {
    std::string copy(text);
    normalizeInPlace(copy);
    return copy;
}

bool TextProcessor::isStopWord(std::string_view token) noexcept {
    return stopWords().contains(token);
}

bool TextProcessor::isIndexable(std::string_view token) noexcept {
    return token.size() >= kMinTokenLength && token.size() <= kMaxTokenLength &&
           !isStopWord(token);
}

std::vector<std::string> TextProcessor::tokenize(std::string_view text) {
    const std::string normalized = normalize(text);
    std::vector<std::string> tokens;
    forEachToken(normalized, [&tokens](std::string_view token) { tokens.emplace_back(token); });
    return tokens;
}

std::vector<std::string> TextProcessor::uniqueTokens(std::string_view text) {
    const std::string normalized = normalize(text);
    std::vector<std::string> tokens;
    std::unordered_set<std::string_view> seen;
    forEachToken(normalized, [&](std::string_view token) {
        if (seen.insert(token).second) {
            tokens.emplace_back(token);
        }
    });
    return tokens;
}

}  // namespace search::core
