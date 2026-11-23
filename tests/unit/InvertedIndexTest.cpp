#include <gtest/gtest.h>

#include "TestUtils.hpp"
#include "index/InvertedIndex.hpp"
#include "threading/ThreadPool.hpp"

using search::index::InvertedIndex;
using search::index::LocalIndex;
using search::index::Posting;
using search::test::TempDir;

TEST(InvertedIndexTest, EmptyIndex) {
    InvertedIndex index;
    EXPECT_EQ(index.documentCount(), 0u);
    EXPECT_EQ(index.termCount(), 0u);
    EXPECT_TRUE(index.postings("anything").empty());
    EXPECT_EQ(index.documentFrequency("anything"), 0u);
    EXPECT_FALSE(index.document(0).has_value());
}

TEST(InvertedIndexTest, RejectsZeroShards) {
    EXPECT_THROW(InvertedIndex index(0), std::invalid_argument);
}

TEST(InvertedIndexTest, AddDocumentBuildsPostingLists) {
    InvertedIndex index;
    const auto d0 = index.addDocument("doc_a", "a.txt", "Neural networks and neural models");
    const auto d1 = index.addDocument("doc_b", "b.txt", "Networks of roads");
    EXPECT_EQ(d0, 0u);
    EXPECT_EQ(d1, 1u);
    EXPECT_EQ(index.documentCount(), 2u);

    EXPECT_EQ(index.documentFrequency("networks"), 2u);
    EXPECT_EQ(index.documentFrequency("neural"), 1u);
    EXPECT_EQ(index.termFrequency("neural", d0), 2u);
    EXPECT_EQ(index.termFrequency("neural", d1), 0u);
    EXPECT_EQ(index.termFrequency("roads", d1), 1u);
    // stop word "and"/"of" are not indexed
    EXPECT_EQ(index.documentFrequency("and"), 0u);
    EXPECT_EQ(index.documentFrequency("of"), 0u);
}

TEST(InvertedIndexTest, DocumentMetadataIsStored) {
    InvertedIndex index;
    const auto id = index.addDocument("document_000007", "/tmp/document_000007.txt", "alpha beta gamma alpha");
    const auto doc = index.document(id);
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->name, "document_000007");
    EXPECT_EQ(doc->path, "/tmp/document_000007.txt");
    EXPECT_EQ(doc->tokenCount, 4u);
    EXPECT_EQ(doc->sizeBytes, 22u);
    ASSERT_TRUE(index.documentByName("document_000007").has_value());
    EXPECT_FALSE(index.documentByName("missing").has_value());
}

TEST(InvertedIndexTest, NonExistentDocumentReturnsNullopt) {
    InvertedIndex index;
    index.addDocument("d", "d.txt", "text");
    EXPECT_FALSE(index.document(42).has_value());
    EXPECT_FALSE(index.documentText(42).has_value());
    EXPECT_EQ(index.snippet(42, {"text"}), "");
}

TEST(InvertedIndexTest, MergeCombinesLocalIndexes) {
    InvertedIndex index(4);
    LocalIndex a;
    a["apple"].push_back({0, 3});
    a["pear"].push_back({0, 1});
    LocalIndex b;
    b["apple"].push_back({1, 2});
    index.merge(std::move(a));
    index.merge(std::move(b));
    index.finalize();
    const auto apple = index.postings("apple");
    ASSERT_EQ(apple.size(), 2u);
    EXPECT_EQ(apple[0], (Posting{0, 3}));
    EXPECT_EQ(apple[1], (Posting{1, 2}));
    EXPECT_EQ(index.postingCount(), 3u);
    EXPECT_EQ(index.termCount(), 2u);
}

TEST(InvertedIndexTest, FinalizeSortsPostingsByDocId) {
    InvertedIndex index(2);
    LocalIndex local;
    local["term"] = {{5, 1}, {1, 1}, {3, 1}};
    index.merge(std::move(local));
    EXPECT_FALSE(index.finalized());
    search::threading::ThreadPool pool(2);
    index.finalize(&pool);
    EXPECT_TRUE(index.finalized());
    const auto list = index.postings("term");
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0].docId, 1u);
    EXPECT_EQ(list[1].docId, 3u);
    EXPECT_EQ(list[2].docId, 5u);
    EXPECT_EQ(index.termFrequency("term", 3), 1u);
    EXPECT_EQ(index.termFrequency("term", 4), 0u);
}

TEST(InvertedIndexTest, FindAllAndFindAny) {
    InvertedIndex index;
    index.addDocument("d0", "d0", "solar energy panels");
    index.addDocument("d1", "d1", "wind energy turbines");
    index.addDocument("d2", "d2", "solar wind astronomy");
    index.finalize();
    EXPECT_EQ(index.findAll({"solar", "energy"}), (std::vector<search::core::DocId>{0}));
    EXPECT_EQ(index.findAll({"solar", "wind"}), (std::vector<search::core::DocId>{2}));
    EXPECT_EQ(index.findAny({"panels", "turbines"}), (std::vector<search::core::DocId>{0, 1}));
    EXPECT_TRUE(index.findAll({"solar", "missing"}).empty());
    EXPECT_TRUE(index.findAll({}).empty());
    EXPECT_EQ(index.findAny({"energy", "astronomy"}).size(), 3u);
}

TEST(InvertedIndexTest, SnippetFromFile) {
    TempDir dir;
    std::string text = "Intro paragraph about cooking recipes. ";
    for (int i = 0; i < 30; ++i) text += "Filler sentence number " + std::to_string(i) + ". ";
    text += "Here the QUANTUM computer appears. More text follows here.";
    const auto path = dir.write("doc.txt", text);
    InvertedIndex index;
    const auto id = index.addDocument("doc", path, text);
    const auto snippet = index.snippet(id, {"quantum"}, 120);
    EXPECT_NE(snippet.find("QUANTUM"), std::string::npos);
    EXPECT_EQ(snippet.rfind("...", 0), 0u);  // starts with ellipsis (cut from the middle)
    EXPECT_LE(snippet.size(), 126u);
}

TEST(InvertedIndexTest, MakeSnippetMatchesWholeWordsOnly) {
    const std::string text = "The carpet was red. Later a car drove by.";
    const auto snippet = InvertedIndex::makeSnippet(text, {"car"}, 20);
    EXPECT_NE(snippet.find("car drove"), std::string::npos);
    EXPECT_EQ(snippet.find("carpet"), std::string::npos);
}

TEST(InvertedIndexTest, MakeSnippetFallsBackToBeginning) {
    const std::string text = "First words of the document. Second sentence.";
    const auto snippet = InvertedIndex::makeSnippet(text, {"absent"}, 15);
    EXPECT_EQ(snippet.rfind("First", 0), 0u);
    EXPECT_NE(snippet.find("..."), std::string::npos);
    EXPECT_EQ(InvertedIndex::makeSnippet("", {"x"}), "");
}

TEST(InvertedIndexTest, MakeSnippetCollapsesWhitespace) {
    const auto snippet = InvertedIndex::makeSnippet("alpha\n\n  beta\tgamma", {"beta"}, 100);
    EXPECT_EQ(snippet, "alpha beta gamma");
}

TEST(InvertedIndexTest, ReplacingDocumentDoesNotDoubleCountTokens) {
    InvertedIndex index;
    index.setDocument({0, "d0", "d0", 10, 100});
    index.setDocument({0, "d0", "d0", 4, 40});
    EXPECT_EQ(index.documentCount(), 1u);
    EXPECT_EQ(index.totalTokens(), 4u);
}

TEST(InvertedIndexTest, DocumentLengthsAreExposed) {
    InvertedIndex index;
    index.addDocument("d0", "d0", "one two three");
    index.addDocument("d1", "d1", "four");
    std::vector<std::uint32_t> lengths;
    index.visitDocumentLengths([&](std::span<const std::uint32_t> l) { lengths.assign(l.begin(), l.end()); });
    EXPECT_EQ(lengths, (std::vector<std::uint32_t>{3, 1}));
    EXPECT_EQ(index.totalTokens(), 4u);
}
