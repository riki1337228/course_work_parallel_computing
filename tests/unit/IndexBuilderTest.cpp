#include <gtest/gtest.h>

#include <map>

#include "TestUtils.hpp"
#include "core/DocumentLoader.hpp"
#include "index/IndexBuilder.hpp"

using search::index::DatasetError;
using search::index::IndexBuilder;
using search::index::InvertedIndex;
using search::test::TempDir;

namespace {

std::string docName(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "document_%06d", i);
    return buf;
}

/// Writes `count` documents; document i contains "common", "topic<i%5>" and "unique<i>".
void writeCorpus(const TempDir& dir, int count) {
    for (int i = 1; i <= count; ++i) {
        std::string text = "Common words appear everywhere. ";
        text += "This text is about topic" + std::to_string(i % 5) + ". ";
        text += "Rare marker unique" + std::to_string(i) + " unique" + std::to_string(i) + ".";
        dir.write("documents/" + docName(i) + ".txt", text);
    }
}

}  // namespace

TEST(IndexBuilderTest, RejectsZeroThreads) {
    EXPECT_THROW(IndexBuilder builder(0), std::invalid_argument);
}

TEST(IndexBuilderTest, IndexesAllDocuments) {
    TempDir dir;
    writeCorpus(dir, 200);
    search::index::BuildStats stats;
    const auto index = IndexBuilder(4).build(dir.path(), &stats);
    EXPECT_EQ(stats.documentsIndexed, 200u);
    EXPECT_EQ(stats.documentsFailed, 0u);
    EXPECT_EQ(stats.threads, 4u);
    EXPECT_GT(stats.tasks, 1u);
    EXPECT_EQ(index->documentCount(), 200u);
    EXPECT_TRUE(index->finalized());
    EXPECT_EQ(index->documentFrequency("common"), 200u);
    EXPECT_EQ(index->documentFrequency("topic0"), 40u);
    EXPECT_EQ(index->documentFrequency("unique17"), 1u);
}

TEST(IndexBuilderTest, DocIdsFollowSortedFileOrder) {
    TempDir dir;
    writeCorpus(dir, 20);
    const auto index = IndexBuilder(3, 2).build(dir.path());
    for (search::core::DocId id = 0; id < 20; ++id) {
        const auto doc = index->document(id);
        ASSERT_TRUE(doc.has_value());
        EXPECT_EQ(doc->name, docName(static_cast<int>(id) + 1));
    }
    const auto doc = index->documentByName("document_000005");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(index->termFrequency("unique5", doc->id), 2u);
}

TEST(IndexBuilderTest, ResultIsIdenticalForAnyThreadCount) {
    TempDir dir;
    writeCorpus(dir, 150);
    const auto reference = IndexBuilder(1).build(dir.path());
    for (std::size_t threads : {2u, 4u, 8u, 16u}) {
        const auto other = IndexBuilder(threads).build(dir.path());
        EXPECT_EQ(other->documentCount(), reference->documentCount());
        EXPECT_EQ(other->termCount(), reference->termCount());
        EXPECT_EQ(other->postingCount(), reference->postingCount());
        for (const char* term : {"common", "topic3", "unique99", "rare"}) {
            EXPECT_EQ(other->postings(term), reference->postings(term)) << term << " threads=" << threads;
        }
    }
}

TEST(IndexBuilderTest, EmptyDatasetThrows) {
    TempDir dir;
    std::filesystem::create_directories(dir.path() / "documents");
    try {
        (void)IndexBuilder(2).build(dir.path());
        FAIL() << "expected DatasetError";
    } catch (const DatasetError& e) {
        EXPECT_EQ(e.kind(), DatasetError::Kind::Empty);
    }
}

TEST(IndexBuilderTest, MissingDatasetThrows) {
    try {
        (void)IndexBuilder(2).build("/definitely/not/existing/dataset");
        FAIL() << "expected DatasetError";
    } catch (const DatasetError& e) {
        EXPECT_EQ(e.kind(), DatasetError::Kind::NotFound);
    }
}

TEST(IndexBuilderTest, MoreThreadsThanDocuments) {
    TempDir dir;
    writeCorpus(dir, 3);
    const auto index = IndexBuilder(16).build(dir.path());
    EXPECT_EQ(index->documentCount(), 3u);
}

TEST(IndexBuilderTest, AutoBatchSizeIsBounded) {
    EXPECT_EQ(IndexBuilder::autoBatchSize(0, 4), 1u);
    EXPECT_EQ(IndexBuilder::autoBatchSize(10, 4), 1u);
    EXPECT_EQ(IndexBuilder::autoBatchSize(20000, 1), 64u);
    EXPECT_EQ(IndexBuilder::autoBatchSize(20000, 16), 64u);
    EXPECT_EQ(IndexBuilder::autoBatchSize(1000, 8), 15u);
}
