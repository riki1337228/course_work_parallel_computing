#include "index/InvertedIndex.hpp"

#include <algorithm>
#include <cctype>
#include <future>
#include <mutex>
#include <stdexcept>

#include "core/DocumentLoader.hpp"
#include "core/TextProcessor.hpp"
#include "threading/ThreadPool.hpp"

namespace search::index {

using core::TextProcessor;

InvertedIndex::InvertedIndex(std::size_t shardCount) {
    if (shardCount == 0) {
        throw std::invalid_argument("InvertedIndex requires at least one shard");
    }
    shards_.reserve(shardCount);
    for (std::size_t i = 0; i < shardCount; ++i) {
        shards_.push_back(std::make_unique<Shard>());
    }
}

std::size_t InvertedIndex::shardIndex(std::string_view term) const noexcept {
    return StringHash{}(term) % shards_.size();
}

InvertedIndex::Shard& InvertedIndex::shardFor(std::string_view term) noexcept {
    return *shards_[shardIndex(term)];
}

const InvertedIndex::Shard& InvertedIndex::shardFor(std::string_view term) const noexcept {
    return *shards_[shardIndex(term)];
}

// ============================================================================ writing

DocId InvertedIndex::addDocument(std::string name, std::filesystem::path path, std::string_view text) {
    std::string normalized(text);
    TextProcessor::normalizeInPlace(normalized);

    std::unordered_map<std::string_view, std::uint32_t> frequencies;
    std::uint32_t tokenCount = 0;
    TextProcessor::forEachToken(normalized, [&](std::string_view token) {
        ++frequencies[token];
        ++tokenCount;
    });

    DocId id = 0;
    {
        std::unique_lock lock(documentsMutex_);
        id = static_cast<DocId>(documents_.size());
        Document doc{id, std::move(name), std::move(path), tokenCount, text.size()};
        nameToId_[doc.name] = id;
        documents_.emplace_back(std::move(doc));
        lengths_.push_back(tokenCount);
        ++documentCount_;
    }
    totalTokens_.fetch_add(tokenCount, std::memory_order_relaxed);

    LocalIndex local;
    local.reserve(frequencies.size());
    for (const auto& [term, tf] : frequencies) {
        local[std::string(term)].push_back(Posting{id, tf});
    }
    merge(std::move(local));
    return id;
}

void InvertedIndex::setDocument(Document document) {
    totalTokens_.fetch_add(document.tokenCount, std::memory_order_relaxed);
    std::unique_lock lock(documentsMutex_);
    if (document.id >= documents_.size()) {
        documents_.resize(static_cast<std::size_t>(document.id) + 1);
        lengths_.resize(documents_.size(), 0);
    }
    lengths_[document.id] = document.tokenCount;
    auto& slot = documents_[document.id];
    if (!slot.has_value()) {
        ++documentCount_;
    }
    nameToId_[document.name] = document.id;
    slot = std::move(document);
}

void InvertedIndex::merge(LocalIndex&& local) {
    if (local.empty()) {
        return;
    }
    // Group the local terms by destination shard so that each shard is locked once.
    std::vector<std::vector<LocalIndex::node_type>> byShard(shards_.size());
    while (!local.empty()) {
        auto node = local.extract(local.begin());
        byShard[shardIndex(node.key())].push_back(std::move(node));
    }

    std::size_t added = 0;
    for (std::size_t s = 0; s < byShard.size(); ++s) {
        auto& nodes = byShard[s];
        if (nodes.empty()) {
            continue;
        }
        Shard& shard = *shards_[s];
        std::unique_lock lock(shard.mutex);  // one short critical section per shard
        for (auto& node : nodes) {
            added += node.mapped().size();
            auto it = shard.terms.find(node.key());
            if (it == shard.terms.end()) {
                shard.terms.insert(std::move(node));  // reuse the node: no copy of postings
            } else {
                auto& target = it->second;
                target.insert(target.end(), std::make_move_iterator(node.mapped().begin()),
                              std::make_move_iterator(node.mapped().end()));
            }
        }
    }
    postingCount_.fetch_add(added, std::memory_order_relaxed);
    finalized_.store(false, std::memory_order_release);
}

void InvertedIndex::finalize(threading::ThreadPool* pool) {
    auto sortShard = [this](std::size_t s) {
        Shard& shard = *shards_[s];
        std::unique_lock lock(shard.mutex);
        for (auto& [term, list] : shard.terms) {
            std::sort(list.begin(), list.end(),
                      [](const Posting& a, const Posting& b) { return a.docId < b.docId; });
            list.shrink_to_fit();  // release growth slack: lists are immutable from now on
        }
    };

    if (pool != nullptr) {
        std::vector<std::future<void>> futures;
        futures.reserve(shards_.size());
        for (std::size_t s = 0; s < shards_.size(); ++s) {
            futures.push_back(pool->submit(sortShard, s));
        }
        for (auto& f : futures) {
            f.get();
        }
    } else {
        for (std::size_t s = 0; s < shards_.size(); ++s) {
            sortShard(s);
        }
    }
    finalized_.store(true, std::memory_order_release);
}

// ============================================================================ reading

bool InvertedIndex::visitPostings(std::string_view term,
                                  const std::function<void(std::span<const Posting>)>& visitor) const {
    const Shard& shard = shardFor(term);
    std::shared_lock lock(shard.mutex);
    const auto it = shard.terms.find(term);
    if (it == shard.terms.end()) {
        return false;
    }
    visitor(std::span<const Posting>(it->second));
    return true;
}

std::vector<Posting> InvertedIndex::postings(std::string_view term) const {
    std::vector<Posting> result;
    visitPostings(term, [&result](std::span<const Posting> list) { result.assign(list.begin(), list.end()); });
    return result;
}

std::size_t InvertedIndex::documentFrequency(std::string_view term) const {
    std::size_t df = 0;
    visitPostings(term, [&df](std::span<const Posting> list) { df = list.size(); });
    return df;
}

std::uint32_t InvertedIndex::termFrequency(std::string_view term, DocId docId) const {
    std::uint32_t tf = 0;
    const bool sorted = finalized();
    visitPostings(term, [&](std::span<const Posting> list) {
        if (sorted) {
            const auto it = std::lower_bound(list.begin(), list.end(), docId,
                                             [](const Posting& p, DocId id) { return p.docId < id; });
            if (it != list.end() && it->docId == docId) {
                tf = it->termFrequency;
            }
        } else {
            for (const auto& p : list) {
                if (p.docId == docId) {
                    tf = p.termFrequency;
                    break;
                }
            }
        }
    });
    return tf;
}

namespace {

std::vector<DocId> sortedDocIds(const InvertedIndex& index, const std::string& term) {
    std::vector<DocId> ids;
    index.visitPostings(term, [&ids](std::span<const Posting> list) {
        ids.reserve(list.size());
        for (const auto& p : list) {
            ids.push_back(p.docId);
        }
    });
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

}  // namespace

std::vector<DocId> InvertedIndex::findAll(const std::vector<std::string>& terms) const {
    if (terms.empty()) {
        return {};
    }
    std::vector<DocId> result = sortedDocIds(*this, terms.front());
    for (std::size_t i = 1; i < terms.size() && !result.empty(); ++i) {
        const std::vector<DocId> next = sortedDocIds(*this, terms[i]);
        std::vector<DocId> intersection;
        std::set_intersection(result.begin(), result.end(), next.begin(), next.end(),
                              std::back_inserter(intersection));
        result = std::move(intersection);
    }
    return result;
}

std::vector<DocId> InvertedIndex::findAny(const std::vector<std::string>& terms) const {
    std::vector<DocId> result;
    for (const auto& term : terms) {
        const std::vector<DocId> next = sortedDocIds(*this, term);
        std::vector<DocId> merged;
        std::set_union(result.begin(), result.end(), next.begin(), next.end(), std::back_inserter(merged));
        result = std::move(merged);
    }
    return result;
}

void InvertedIndex::visitDocumentLengths(
    const std::function<void(std::span<const std::uint32_t>)>& visitor) const {
    std::shared_lock lock(documentsMutex_);
    visitor(std::span<const std::uint32_t>(lengths_));
}

std::optional<Document> InvertedIndex::document(DocId id) const {
    std::shared_lock lock(documentsMutex_);
    if (id >= documents_.size()) {
        return std::nullopt;
    }
    return documents_[id];
}

std::optional<Document> InvertedIndex::documentByName(std::string_view name) const {
    std::shared_lock lock(documentsMutex_);
    const auto it = nameToId_.find(name);
    if (it == nameToId_.end()) {
        return std::nullopt;
    }
    return documents_[it->second];
}

std::optional<std::string> InvertedIndex::documentText(DocId id) const {
    const auto doc = document(id);
    if (!doc) {
        return std::nullopt;
    }
    try {
        return core::DocumentLoader::readFile(doc->path);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string InvertedIndex::snippet(DocId id, const std::vector<std::string>& terms, std::size_t maxLength) const {
    const auto text = documentText(id);
    if (!text) {
        return {};
    }
    return makeSnippet(*text, terms, maxLength);
}

std::string InvertedIndex::makeSnippet(std::string_view text, const std::vector<std::string>& terms,
                                       std::size_t maxLength) {
    if (text.empty() || maxLength == 0) {
        return {};
    }
    // Normalization is byte-to-byte, so positions in `normalized` map 1:1 to `text`.
    const std::string normalized = TextProcessor::normalize(text);

    std::size_t best = std::string::npos;
    for (const auto& rawTerm : terms) {
        const std::string term = TextProcessor::normalize(rawTerm);
        if (term.empty() || term.find(' ') != std::string::npos) {
            continue;
        }
        std::size_t pos = normalized.find(term);
        while (pos != std::string::npos && pos < best) {
            const bool leftOk = pos == 0 || normalized[pos - 1] == ' ';
            const std::size_t end = pos + term.size();
            const bool rightOk = end >= normalized.size() || normalized[end] == ' ';
            if (leftOk && rightOk) {
                best = pos;
                break;
            }
            pos = normalized.find(term, pos + 1);
        }
    }

    std::size_t start = 0;
    if (best != std::string::npos && best > maxLength / 3) {
        start = best - maxLength / 3;
        // move forward to the next word boundary
        while (start < best && text[start] != ' ' && text[start] != '\n') {
            ++start;
        }
    }
    std::size_t end = std::min(text.size(), start + maxLength);
    if (end < text.size()) {
        std::size_t cut = end;
        while (cut > start && text[cut] != ' ' && text[cut] != '\n') {
            --cut;
        }
        if (cut > start) {
            end = cut;
        }
    }

    std::string result;
    result.reserve(end - start + 6);
    if (start > 0) {
        result += "...";
    }
    bool lastSpace = start > 0;
    for (std::size_t i = start; i < end; ++i) {
        const char ch = text[i];
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!lastSpace) {
                result.push_back(' ');
            }
            lastSpace = true;
        } else {
            result.push_back(ch);
            lastSpace = false;
        }
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    if (end < text.size()) {
        result += "...";
    }
    return result;
}

std::size_t InvertedIndex::documentCount() const {
    std::shared_lock lock(documentsMutex_);
    return documentCount_;
}

std::size_t InvertedIndex::termCount() const {
    std::size_t total = 0;
    for (const auto& shard : shards_) {
        std::shared_lock lock(shard->mutex);
        total += shard->terms.size();
    }
    return total;
}

}  // namespace search::index
