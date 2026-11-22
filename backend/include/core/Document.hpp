#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace search::core {

/// Internal dense document identifier (position in the document table).
using DocId = std::uint32_t;

/// Metadata of one indexed document. The full text is not kept in memory;
/// it can always be re-read from `path` (see InvertedIndex::documentText()).
struct Document {
    DocId id = 0;                    ///< internal numeric id
    std::string name;                ///< public id, e.g. "document_000001" (file stem)
    std::filesystem::path path;      ///< absolute or relative path to the .txt file
    std::uint32_t tokenCount = 0;    ///< number of indexable tokens (|d| in TF formula)
    std::uint64_t sizeBytes = 0;     ///< file size in bytes
};

}  // namespace search::core
