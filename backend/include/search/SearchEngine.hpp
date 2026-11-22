#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "index/InvertedIndex.hpp"

namespace search::engine {

/// One weighted term of a search query.
struct QueryTerm {
    std::string term;      ///< normalized token
    double weight = 1.0;   ///< 1.0 for original terms, Config::expandedTermWeight for LLM terms
    bool expanded = false; ///< true if the term came from query expansion
};

struct SearchResult {
    std::string docId;     ///< public document id (file stem)
    double score = 0.0;    ///< TF-IDF relevance score
    std::string snippet;
};

struct SearchResponse {
    std::vector<SearchResult> results;  ///< sorted by score, descending
    std::size_t totalMatches = 0;       ///< documents with score > 0
    double timeMs = 0.0;                ///< index lookup + ranking + snippets
};

/// Thrown when a search is executed before any index has been built.
class IndexNotReadyError : public std::runtime_error {
public:
    IndexNotReadyError() : std::runtime_error("index is not built yet") {}
};

/// Keyword search with TF-IDF ranking on top of an immutable InvertedIndex snapshot.
///
///   score(d, Q) = sum_{t in Q} w_t * tf(t, d) * idf(t)
///   tf(t, d)    = f(t, d) / |d|               (term count normalized by document length)
///   idf(t)      = ln(N / df(t)) + 1           (N = indexed documents, df = documents with t)
///
/// Multi-term queries use OR semantics: a document matches if it contains at least
/// one term; documents containing more (and rarer) terms get higher scores.
///
/// The current index is held in a std::shared_ptr. A rebuild publishes a new
/// snapshot with setIndex(); running searches keep using the old snapshot until
/// they finish, so searching never blocks on indexing.
class SearchEngine {
public:
    void setIndex(std::shared_ptr<const index::InvertedIndex> index);
    [[nodiscard]] std::shared_ptr<const index::InvertedIndex> index() const;
    [[nodiscard]] bool ready() const;

    /// Throws IndexNotReadyError if no index has been published.
    [[nodiscard]] SearchResponse search(const std::vector<QueryTerm>& terms, std::size_t limit) const;

    /// Ranking on a given index (stateless; used by search() and unit tests).
    static SearchResponse rank(const index::InvertedIndex& index, const std::vector<QueryTerm>& terms,
                               std::size_t limit, bool withSnippets = true);

    /// Builds weighted, de-duplicated query terms from the original query and
    /// expansion phrases. Original terms win over duplicates from the expansion.
    static std::vector<QueryTerm> buildQueryTerms(std::string_view originalQuery,
                                                  const std::vector<std::string>& expandedPhrases,
                                                  double expandedWeight);

    /// idf(t) = ln(N / df) + 1; 0 when df == 0.
    static double inverseDocumentFrequency(std::size_t totalDocuments, std::size_t documentFrequency);

private:
    mutable std::mutex mutex_;  // protects the shared_ptr itself (not the index)
    std::shared_ptr<const index::InvertedIndex> index_;
};

}  // namespace search::engine
