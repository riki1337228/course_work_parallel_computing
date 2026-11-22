#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/Document.hpp"

namespace search::threading {
class ThreadPool;
}

namespace search::index {

using core::DocId;
using core::Document;

/// One entry of a posting list: document and number of occurrences of the term in it.
struct Posting {
    DocId docId = 0;
    std::uint32_t termFrequency = 0;

    friend bool operator==(const Posting&, const Posting&) = default;
};

/// Transparent hash so that maps keyed by std::string can be queried with std::string_view.
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

using PostingMap = std::unordered_map<std::string, std::vector<Posting>, StringHash, std::equal_to<>>;

/// Thread-local partial index built by one worker for a batch of documents.
/// It is merged into the shared InvertedIndex with InvertedIndex::merge().
using LocalIndex = PostingMap;

/// Custom thread-safe inverted index: term -> posting list (doc id + term frequency).
///
/// Concurrency design (lock striping):
///  - the term dictionary is split into `shardCount` shards by hash(term);
///  - every shard has its own std::shared_mutex: writers (merge/addDocument) take a
///    unique lock on one shard at a time, readers (search) take shared locks, so
///    threads that touch different shards never contend;
///  - the document table is protected by a separate std::shared_mutex;
///  - counters are std::atomic.
/// A writer never holds two shard locks at once, therefore deadlock is impossible.
class InvertedIndex {
public:
    static constexpr std::size_t kDefaultShardCount = 64;

    explicit InvertedIndex(std::size_t shardCount = kDefaultShardCount);

    InvertedIndex(const InvertedIndex&) = delete;
    InvertedIndex& operator=(const InvertedIndex&) = delete;

    // ---------------------------------------------------------------- writing

    /// Tokenizes `text` and adds a new document. Thread-safe. Returns its DocId.
    DocId addDocument(std::string name, std::filesystem::path path, std::string_view text);

    /// Stores metadata for a document whose id was pre-assigned (used by IndexBuilder).
    /// Thread-safe; grows the document table if needed.
    void setDocument(Document document);

    /// Merges a thread-local partial index. Postings are moved; every shard is
    /// locked exactly once per call. Thread-safe.
    void merge(LocalIndex&& local);

    /// Sorts every posting list by doc id (optionally in parallel on `pool`) and
    /// marks the index as finalized. Call after all merges are done.
    void finalize(threading::ThreadPool* pool = nullptr);

    // ---------------------------------------------------------------- reading

    /// Invokes `visitor` with the posting list of `term` while holding a shared
    /// lock on its shard. Returns false if the term is not indexed.
    bool visitPostings(std::string_view term,
                       const std::function<void(std::span<const Posting>)>& visitor) const;

    /// Copy of the posting list of `term` (empty if absent).
    [[nodiscard]] std::vector<Posting> postings(std::string_view term) const;

    /// Number of documents containing `term` (df).
    [[nodiscard]] std::size_t documentFrequency(std::string_view term) const;

    /// Occurrences of `term` in document `docId` (0 if none).
    [[nodiscard]] std::uint32_t termFrequency(std::string_view term, DocId docId) const;

    /// Documents that contain ALL terms (AND), sorted by doc id.
    [[nodiscard]] std::vector<DocId> findAll(const std::vector<std::string>& terms) const;

    /// Documents that contain ANY of the terms (OR), sorted by doc id.
    [[nodiscard]] std::vector<DocId> findAny(const std::vector<std::string>& terms) const;

    /// Invokes `visitor` with the table of document lengths (|d|, indexed by DocId)
    /// while holding a shared lock on the document table. Used by ranking to avoid
    /// a lock + copy per posting.
    void visitDocumentLengths(const std::function<void(std::span<const std::uint32_t>)>& visitor) const;

    [[nodiscard]] std::optional<Document> document(DocId id) const;
    [[nodiscard]] std::optional<Document> documentByName(std::string_view name) const;

    /// Full text of a document, re-read from disk. std::nullopt if unknown/unreadable.
    [[nodiscard]] std::optional<std::string> documentText(DocId id) const;

    /// Text fragment around the first occurrence of any of `terms` (whole-word,
    /// case-insensitive). Falls back to the beginning of the document.
    [[nodiscard]] std::string snippet(DocId id, const std::vector<std::string>& terms,
                                      std::size_t maxLength = 240) const;

    /// Snippet extraction on an in-memory text (exposed for unit tests).
    static std::string makeSnippet(std::string_view text, const std::vector<std::string>& terms,
                                   std::size_t maxLength = 240);

    [[nodiscard]] std::size_t documentCount() const;
    [[nodiscard]] std::size_t termCount() const;
    [[nodiscard]] std::size_t postingCount() const noexcept { return postingCount_.load(); }
    [[nodiscard]] std::uint64_t totalTokens() const noexcept { return totalTokens_.load(); }
    [[nodiscard]] bool finalized() const noexcept { return finalized_.load(); }
    [[nodiscard]] std::size_t shardCount() const noexcept { return shards_.size(); }

private:
    struct Shard {
        mutable std::shared_mutex mutex;
        PostingMap terms;
    };

    [[nodiscard]] std::size_t shardIndex(std::string_view term) const noexcept;
    [[nodiscard]] Shard& shardFor(std::string_view term) noexcept;
    [[nodiscard]] const Shard& shardFor(std::string_view term) const noexcept;

    std::vector<std::unique_ptr<Shard>> shards_;

    mutable std::shared_mutex documentsMutex_;
    std::vector<std::optional<Document>> documents_;
    std::vector<std::uint32_t> lengths_;  // |d| per DocId, 0 for missing documents
    std::unordered_map<std::string, DocId, StringHash, std::equal_to<>> nameToId_;
    std::size_t documentCount_ = 0;  // guarded by documentsMutex_

    std::atomic<std::size_t> postingCount_{0};
    std::atomic<std::uint64_t> totalTokens_{0};
    std::atomic<bool> finalized_{false};
};

}  // namespace search::index
