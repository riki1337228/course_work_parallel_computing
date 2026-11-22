// Concurrency tests. They are also executed under ThreadSanitizer
// (Docker target `tsan`), which reports any data race as a test failure.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "TestUtils.hpp"
#include "index/IndexBuilder.hpp"
#include "index/InvertedIndex.hpp"
#include "search/SearchEngine.hpp"
#include "search/SearchService.hpp"
#include "threading/ThreadPool.hpp"

using search::engine::QueryTerm;
using search::engine::SearchEngine;
using search::index::IndexBuilder;
using search::index::InvertedIndex;
using search::test::FakeLlmClient;
using search::test::TempDir;

namespace {

void writeCorpus(const TempDir& dir, int count) {
    for (int i = 1; i <= count; ++i) {
        char name[48];
        std::snprintf(name, sizeof(name), "documents/document_%06d.txt", i);
        std::string text = "shared vocabulary term alpha beta. ";
        text += "group" + std::to_string(i % 7) + " marker" + std::to_string(i) + ". ";
        for (int k = 0; k < i % 5; ++k) text += "repeat ";
        dir.write(name, text);
    }
}

}  // namespace

TEST(ConcurrencyTest, ConcurrentAddDocumentLosesNothing) {
    InvertedIndex index(16);
    constexpr int kThreads = 8;
    constexpr int kPerThread = 250;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&index, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const std::string name = "t" + std::to_string(t) + "_" + std::to_string(i);
                index.addDocument(name, name, "common thread" + std::to_string(t) + " doc " + name);
            }
        });
    }
    for (auto& th : threads) th.join();
    index.finalize();

    EXPECT_EQ(index.documentCount(), static_cast<std::size_t>(kThreads * kPerThread));
    EXPECT_EQ(index.documentFrequency("common"), static_cast<std::size_t>(kThreads * kPerThread));
    for (int t = 0; t < kThreads; ++t) {
        EXPECT_EQ(index.documentFrequency("thread" + std::to_string(t)), static_cast<std::size_t>(kPerThread));
    }
    // Every doc id appears exactly once in the "common" posting list.
    const auto list = index.postings("common");
    for (std::size_t i = 0; i < list.size(); ++i) {
        EXPECT_EQ(list[i].docId, i);
    }
}

TEST(ConcurrencyTest, ParallelBuildMatchesSequentialBuild) {
    TempDir dir;
    writeCorpus(dir, 700);
    const auto sequential = IndexBuilder(1).build(dir.path());
    const auto parallel = IndexBuilder(8, 5).build(dir.path());
    EXPECT_EQ(parallel->documentCount(), 700u);
    EXPECT_EQ(parallel->postingCount(), sequential->postingCount());
    EXPECT_EQ(parallel->termCount(), sequential->termCount());
    for (const char* term : {"shared", "group3", "marker512", "repeat"}) {
        EXPECT_EQ(parallel->postings(term), sequential->postings(term)) << term;
    }
}

TEST(ConcurrencyTest, ConcurrentReadersGetConsistentResults) {
    TempDir dir;
    writeCorpus(dir, 300);
    std::shared_ptr<const InvertedIndex> index = IndexBuilder(4).build(dir.path());
    const std::vector<QueryTerm> query = {{"group3", 1.0, false}, {"repeat", 1.0, false}};
    const auto expected = SearchEngine::rank(*index, query, 20, false);

    std::atomic<int> mismatches{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 12; ++r) {
        readers.emplace_back([&] {
            for (int i = 0; i < 100; ++i) {
                const auto got = SearchEngine::rank(*index, query, 20, false);
                if (got.results.size() != expected.results.size() || got.totalMatches != expected.totalMatches) {
                    ++mismatches;
                    continue;
                }
                for (std::size_t k = 0; k < got.results.size(); ++k) {
                    if (got.results[k].docId != expected.results[k].docId) ++mismatches;
                }
                (void)index->documentFrequency("shared");
                (void)index->termFrequency("marker7", 6);
            }
        });
    }
    for (auto& th : readers) th.join();
    EXPECT_EQ(mismatches.load(), 0);
}

TEST(ConcurrencyTest, ReadersAndWritersOnSameIndex) {
    InvertedIndex index(8);
    for (int i = 0; i < 50; ++i) index.addDocument("seed" + std::to_string(i), "", "seed common");
    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                const std::vector<QueryTerm> q = {{"common", 1.0, false}};
                const auto res = SearchEngine::rank(index, q, 5, false);
                EXPECT_GE(res.totalMatches, 50u);
                ++reads;
            }
        });
    }
    std::vector<std::thread> writers;
    for (int w = 0; w < 4; ++w) {
        writers.emplace_back([&index, w] {
            for (int i = 0; i < 200; ++i) {
                index.addDocument("w" + std::to_string(w) + "_" + std::to_string(i), "", "common writer");
            }
        });
    }
    for (auto& th : writers) th.join();
    stop = true;
    for (auto& th : readers) th.join();

    EXPECT_EQ(index.documentCount(), 850u);
    EXPECT_EQ(index.documentFrequency("common"), 850u);
    EXPECT_GT(reads.load(), 0);
}

TEST(ConcurrencyTest, ConcurrentSearchesDuringRebuild) {
    TempDir dir;
    writeCorpus(dir, 400);
    search::core::Config cfg;
    cfg.datasetPath = dir.path().string();
    cfg.requestThreads = 4;
    auto llm = std::make_shared<FakeLlmClient>();
    llm->setResponse(R"({"expanded_terms":["alpha","beta"]})");
    search::engine::SearchService service(cfg, llm);
    ASSERT_EQ(service.buildIndex(4).error, search::engine::IndexBuildResult::Error::None);

    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};
    std::atomic<int> searches{0};
    std::vector<std::thread> clients;
    for (int c = 0; c < 8; ++c) {
        clients.emplace_back([&, c] {
            while (!stop.load()) {
                try {
                    const auto result = service.runPipeline("group" + std::to_string(c % 7), 10, c % 2 == 0);
                    if (result.search.results.empty()) ++failures;
                    ++searches;
                } catch (...) {
                    ++failures;
                }
            }
        });
    }
    for (int i = 0; i < 3; ++i) {
        const auto r = service.buildIndex(2 + i);
        EXPECT_EQ(r.error, search::engine::IndexBuildResult::Error::None);
        EXPECT_EQ(r.documentsIndexed, 400u);
    }
    stop = true;
    for (auto& th : clients) th.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_GT(searches.load(), 0);
    EXPECT_EQ(service.stats().searchesServed, static_cast<std::size_t>(searches.load()));
}

TEST(ConcurrencyTest, OnlyOneIndexBuildAtATime) {
    TempDir dir;
    writeCorpus(dir, 600);
    search::core::Config cfg;
    cfg.datasetPath = dir.path().string();
    cfg.requestThreads = 1;
    search::engine::SearchService service(cfg, nullptr);

    std::atomic<int> succeeded{0};
    std::atomic<int> rejected{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 6; ++i) {
        callers.emplace_back([&] {
            const auto r = service.buildIndex(2);
            if (r.error == search::engine::IndexBuildResult::Error::None) ++succeeded;
            if (r.error == search::engine::IndexBuildResult::Error::InProgress) ++rejected;
        });
    }
    for (auto& th : callers) th.join();
    EXPECT_GE(succeeded.load(), 1);
    EXPECT_EQ(succeeded.load() + rejected.load(), 6);
    EXPECT_FALSE(service.indexing());
}

TEST(ConcurrencyTest, ThreadPoolSubmitFromManyThreadsWithResults) {
    search::threading::ThreadPool pool(6);
    std::vector<std::thread> producers;
    std::atomic<long long> sum{0};
    for (int p = 0; p < 6; ++p) {
        producers.emplace_back([&pool, &sum, p] {
            std::vector<std::future<int>> futures;
            for (int i = 0; i < 500; ++i) {
                futures.push_back(pool.submit([p, i] { return p * 1000 + i; }));
            }
            for (auto& f : futures) sum += f.get();
        });
    }
    for (auto& th : producers) th.join();
    long long expected = 0;
    for (int p = 0; p < 6; ++p)
        for (int i = 0; i < 500; ++i) expected += p * 1000 + i;
    EXPECT_EQ(sum.load(), expected);
}
