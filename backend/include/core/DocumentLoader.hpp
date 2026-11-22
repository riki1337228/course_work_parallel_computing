#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace search::core {

/// Discovers and reads dataset documents from disk.
///
/// The dataset root may either contain a `documents/` sub-directory (layout
/// produced by scripts/generate_dataset.py) or the .txt files directly.
class DocumentLoader {
public:
    explicit DocumentLoader(std::filesystem::path datasetRoot);

    /// Directory that actually contains the .txt files.
    [[nodiscard]] std::filesystem::path documentsDirectory() const;

    /// True if the documents directory exists.
    [[nodiscard]] bool exists() const;

    /// All regular *.txt files, sorted by file name (deterministic doc ids).
    /// Throws std::runtime_error if the directory does not exist.
    [[nodiscard]] std::vector<std::filesystem::path> listDocuments() const;

    /// Reads the whole file. Throws std::runtime_error on I/O error.
    static std::string readFile(const std::filesystem::path& path);

    /// Public document id derived from the file name ("document_000001.txt" -> "document_000001").
    static std::string documentName(const std::filesystem::path& path);

private:
    std::filesystem::path root_;
};

}  // namespace search::core
