#include "search/SearchEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

#include "core/TextProcessor.hpp"

namespace search::engine {

void SearchEngine::setIndex(std::shared_ptr<const index::InvertedIndex> index) {
    std::lock_guard lock(mutex_);
    index_ = std::move(index);
}

std::shared_ptr<const index::InvertedIndex> SearchEngine::index() const {
    std::lock_guard lock(mutex_);
    return index_;
}

bool SearchEngine::ready() const {
    return index() != nullptr;
}

SearchResponse SearchEngine::search(const std::vector<QueryTerm>& terms, std::size_t limit) const {
    const auto snapshot = index();  // keeps the index alive for the whole search
    if (!snapshot) {
        throw IndexNotReadyError();
    }
    return rank(*snapshot, terms, limit, true);
}

double SearchEngine::inverseDocumentFrequency(std::size_t totalDocuments, std::size_t documentFrequency) {
    if (documentFrequency == 0 || totalDocuments == 0) {
        return 0.0;
    }
    return std::log(static_cast<double>(totalDocuments) / static_cast<double>(documentFrequency)) + 1.0;
}

SearchResponse SearchEngine::rank(const index::InvertedIndex& index, const std::vector<QueryTerm>& terms,
                                  std::size_t limit, bool withSnippets) {
    const auto start = std::chrono::steady_clock::now();
    SearchResponse response;

    const std::size_t totalDocs = index.documentCount();
    if (!terms.empty() && totalDocs > 0) {
        // Score accumulator keyed by doc id. A hash map keeps memory proportional to
        // the number of matching documents rather than to the size of the corpus.
        std::unordered_map<core::DocId, double> scores;

        index.visitDocumentLengths([&](std::span<const std::uint32_t> lengths) {
            for (const auto& qt : terms) {
                index.visitPostings(qt.term, [&](std::span<const index::Posting> list) {
                    const double idf = inverseDocumentFrequency(totalDocs, list.size());
                    scores.reserve(scores.size() + list.size());
                    for (const auto& posting : list) {
                        if (posting.docId >= lengths.size() || lengths[posting.docId] == 0) {
                            continue;
                        }
                        const double tf = static_cast<double>(posting.termFrequency) / lengths[posting.docId];
                        scores[posting.docId] += qt.weight * tf * idf;
                    }
                });
            }
        });

        std::vector<std::pair<core::DocId, double>> ranked(scores.begin(), scores.end());
        response.totalMatches = ranked.size();
        const std::size_t topK = std::min(limit, ranked.size());
        auto byScore = [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        };
        std::partial_sort(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(topK), ranked.end(), byScore);

        std::vector<std::string> snippetTerms;
        snippetTerms.reserve(terms.size());
        for (const auto& qt : terms) {  // original terms first: they are preferred in snippets
            if (!qt.expanded) snippetTerms.push_back(qt.term);
        }
        for (const auto& qt : terms) {
            if (qt.expanded) snippetTerms.push_back(qt.term);
        }

        response.results.reserve(topK);
        for (std::size_t i = 0; i < topK; ++i) {
            const auto doc = index.document(ranked[i].first);
            SearchResult result;
            result.docId = doc ? doc->name : std::to_string(ranked[i].first);
            result.score = ranked[i].second;
            if (withSnippets) {
                result.snippet = index.snippet(ranked[i].first, snippetTerms);
            }
            response.results.push_back(std::move(result));
        }
    }

    response.timeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return response;
}

std::vector<QueryTerm> SearchEngine::buildQueryTerms(std::string_view originalQuery,
                                                     const std::vector<std::string>& expandedPhrases,
                                                     double expandedWeight) {
    std::vector<QueryTerm> terms;
    std::unordered_map<std::string, std::size_t> position;

    for (auto& token : core::TextProcessor::uniqueTokens(originalQuery)) {
        position.emplace(token, terms.size());
        terms.push_back(QueryTerm{std::move(token), 1.0, false});
    }
    for (const auto& phrase : expandedPhrases) {
        for (auto& token : core::TextProcessor::uniqueTokens(phrase)) {
            if (position.contains(token)) {
                continue;  // already present (original terms keep their full weight)
            }
            position.emplace(token, terms.size());
            terms.push_back(QueryTerm{std::move(token), expandedWeight, true});
        }
    }
    return terms;
}

}  // namespace search::engine
