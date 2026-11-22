#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include "index/InvertedIndex.hpp"

namespace search::index {

/// Result of one parallel index build.
struct BuildStats {
    std::size_t documentsIndexed = 0;
    std::size_t documentsFailed = 0;  ///< files that could not be read
    std::size_t threads = 0;
    std::size_t tasks = 0;            ///< number of batches submitted to the pool
    std::size_t batchSize = 0;
    std::uint64_t bytesRead = 0;
    double listTimeMs = 0.0;          ///< directory listing
    double processTimeMs = 0.0;       ///< parallel read + tokenize + merge
    double finalizeTimeMs = 0.0;      ///< parallel sort of posting lists
    double totalTimeMs = 0.0;
};

/// Error raised when the dataset is missing or contains no documents.
class DatasetError : public std::runtime_error {
public:
    enum class Kind { NotFound, Empty };
    DatasetError(Kind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}
    [[nodiscard]] Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

/// Builds an InvertedIndex from a directory of .txt files using a ThreadPool.
///
/// Algorithm (data parallelism, "map + merge"):
///  1. list and sort all files; document i gets DocId i (deterministic);
///  2. split the list into batches of `batchSize` documents and submit one task per
///     batch to a ThreadPool with N workers (dynamic load balancing via the queue);
///  3. each task reads its files, normalizes and tokenizes them and builds a private
///     LocalIndex (no synchronization at all during this phase);
///  4. at the end of the batch the LocalIndex is merged into the shared index:
///     every shard is locked once, so contention is limited to short merges;
///  5. posting lists are sorted in parallel (finalize).
class IndexBuilder {
public:
    /// `batchSize == 0` selects an automatic batch size.
    explicit IndexBuilder(std::size_t threads, std::size_t batchSize = 0,
                          std::size_t shardCount = InvertedIndex::kDefaultShardCount);

    /// Builds a new index. Throws DatasetError if the dataset is missing/empty.
    std::shared_ptr<InvertedIndex> build(const std::filesystem::path& datasetRoot, BuildStats* stats = nullptr) const;

    /// Batch size used for `documentCount` documents and `threads` workers.
    static std::size_t autoBatchSize(std::size_t documentCount, std::size_t threads);

private:
    std::size_t threads_;
    std::size_t batchSize_;
    std::size_t shardCount_;
};

}  // namespace search::index
