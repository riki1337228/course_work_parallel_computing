#include <gtest/gtest.h>

#include "TestUtils.hpp"
#include "core/DocumentLoader.hpp"

using search::core::DocumentLoader;
using search::test::TempDir;

TEST(DocumentLoaderTest, UsesDocumentsSubdirectoryWhenPresent) {
    TempDir dir;
    dir.write("documents/document_000002.txt", "two");
    dir.write("documents/document_000001.txt", "one");
    dir.write("metadata.json", "{}");
    DocumentLoader loader(dir.path());
    EXPECT_EQ(loader.documentsDirectory(), dir.path() / "documents");
    const auto files = loader.listDocuments();
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(DocumentLoader::documentName(files[0]), "document_000001");
    EXPECT_EQ(DocumentLoader::documentName(files[1]), "document_000002");
}

TEST(DocumentLoaderTest, ListsOnlyTxtFilesSorted) {
    TempDir dir;
    dir.write("b.txt", "b");
    dir.write("a.txt", "a");
    dir.write("notes.md", "ignored");
    dir.write("data.json", "ignored");
    DocumentLoader loader(dir.path());
    const auto files = loader.listDocuments();
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].filename(), "a.txt");
    EXPECT_EQ(files[1].filename(), "b.txt");
}

TEST(DocumentLoaderTest, ReadFileReturnsExactContent) {
    TempDir dir;
    const std::string content = "line one\nline two\n\twith tab";
    const auto path = dir.write("doc.txt", content);
    EXPECT_EQ(DocumentLoader::readFile(path), content);
}

TEST(DocumentLoaderTest, ReadEmptyFile) {
    TempDir dir;
    const auto path = dir.write("empty.txt", "");
    EXPECT_EQ(DocumentLoader::readFile(path), "");
}

TEST(DocumentLoaderTest, MissingDirectoryIsReported) {
    DocumentLoader loader("/definitely/not/existing/dataset");
    EXPECT_FALSE(loader.exists());
    EXPECT_THROW((void)loader.listDocuments(), std::runtime_error);
}

TEST(DocumentLoaderTest, MissingFileThrows) {
    EXPECT_THROW(DocumentLoader::readFile("/definitely/not/existing.txt"), std::runtime_error);
}

TEST(DocumentLoaderTest, EmptyDirectoryHasNoDocuments) {
    TempDir dir;
    DocumentLoader loader(dir.path());
    EXPECT_TRUE(loader.exists());
    EXPECT_TRUE(loader.listDocuments().empty());
}
