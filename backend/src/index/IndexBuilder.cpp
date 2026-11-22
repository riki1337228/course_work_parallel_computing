#include "index/IndexBuilder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <unordered_map>
#include <vector>

#include "core/DocumentLoader.hpp"
#include "core/TextProcessor.hpp"
#include "threading/ThreadPool.hpp"

namespace search::index {
namespace {

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

struct BatchResult {
    std::size_t indexed = 0;
    std::size_t failed = 0;
    std::uint64_t bytes = 0;
};

/// Processes documents [begin, end) of `files` and merges them into `index`.
BatchResult processBatch(const std::vector<std::filesystem::path>& files, std::size_t begin, std::size_t end,
                         InvertedIndex& index) {
    BatchResult result;
    LocalIndex local;  // private to this task: no locking while tokenizing
    std::unordered_map<std::string_view, std::uint32_t> frequencies;

    for (std::size_t i = begin; i < end; ++i) {
        const auto docId = static_cast<DocId>(i);
        std::string text;
        try {
            text = core::DocumentLoader::readFile(files[i]);
        } catch (const std::exception&) {
            ++result.failed;
            continue;
        }
        const std::uint64_t sizeBytes = text.size();
        core::TextProcessor::normalizeInPlace(text);

        frequencies.clear();
        std::uint32_t tokenCount = 0;
        core::TextProcessor::forEachToken(text, [&](std::string_view token) {
            ++frequencies[token];
            ++tokenCount;
        });
        for (const auto& [term, tf] : frequencies) {
            auto it = local.find(term);
            if (it == local.end()) {
                it = local.emplace(std::string(term), std::vector<Posting>{}).first;
            }
            it->second.push_back(Posting{docId, tf});
        }

        index.setDocument(Document{docId, core::DocumentLoader::documentName(files[i]), files[i], tokenCount, sizeBytes});
        ++result.indexed;
        result.bytes += sizeBytes;
    }

    index.merge(std::move(local));  // the only point where shards are locked for writing
    return result;
}

}  // namespace

IndexBuilder::IndexBuilder(std::size_t threads, std::size_t batchSize, std::size_t shardCount)
    : threads_(threads), batchSize_(batchSize), shardCount_(shardCount) {
    if (threads_ == 0) {
        throw std::invalid_argument("IndexBuilder requires at least one thread");
    }
}

std::size_t IndexBuilder::autoBatchSize(std::size_t documentCount, std::size_t threads) {
    // ~8 tasks per worker gives good load balancing; cap keeps LocalIndex small.
    const std::size_t target = documentCount / (std::max<std::size_t>(1, threads) * 8);
    return std::clamp<std::size_t>(target, 1, 64);
}

std::shared_ptr<InvertedIndex> IndexBuilder::build(const std::filesystem::path& datasetRoot, BuildStats* stats) const {
    const auto t0 = Clock::now();
    const core::DocumentLoader loader(datasetRoot);
    if (!loader.exists()) {
        throw DatasetError(DatasetError::Kind::NotFound,
                           "dataset directory not found: " + loader.documentsDirectory().string());
    }
    const std::vector<std::filesystem::path> files = loader.listDocuments();
    if (files.empty()) {
        throw DatasetError(DatasetError::Kind::Empty,
                           "dataset directory contains no .txt documents: " + loader.documentsDirectory().string());
    }
    const auto t1 = Clock::now();

    auto index = std::make_shared<InvertedIndex>(shardCount_);
    const std::size_t batch = batchSize_ > 0 ? batchSize_ : autoBatchSize(files.size(), threads_);

    BuildStats local;
    local.threads = threads_;
    local.batchSize = batch;

    threading::ThreadPool pool(threads_);
    std::vector<std::future<BatchResult>> futures;
    futures.reserve(files.size() / batch + 1);
    for (std::size_t begin = 0; begin < files.size(); begin += batch) {
        const std::size_t end = std::min(files.size(), begin + batch);
        futures.push_back(pool.submit([&files, begin, end, &index] { return processBatch(files, begin, end, *index); }));
    }
    local.tasks = futures.size();

    for (auto& f : futures) {
        const BatchResult r = f.get();  // rethrows unexpected task exceptions
        local.documentsIndexed += r.indexed;
        local.documentsFailed += r.failed;
        local.bytesRead += r.bytes;
    }
    const auto t2 = Clock::now();

    index->finalize(&pool);
    pool.shutdown();
    const auto t3 = Clock::now();

    local.listTimeMs = elapsedMs(t0, t1);
    local.processTimeMs = elapsedMs(t1, t2);
    local.finalizeTimeMs = elapsedMs(t2, t3);
    local.totalTimeMs = elapsedMs(t0, t3);
    if (stats != nullptr) {
        *stats = local;
    }
    return index;
}

}  // namespace search::index
