#include "core/DocumentLoader.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace search::core {

DocumentLoader::DocumentLoader(fs::path datasetRoot) : root_(std::move(datasetRoot)) {}

fs::path DocumentLoader::documentsDirectory() const {
    std::error_code ec;
    const fs::path nested = root_ / "documents";
    if (fs::is_directory(nested, ec)) {
        return nested;
    }
    return root_;
}

bool DocumentLoader::exists() const {
    std::error_code ec;
    return fs::is_directory(documentsDirectory(), ec);
}

std::vector<fs::path> DocumentLoader::listDocuments() const {
    const fs::path dir = documentsDirectory();
    if (!exists()) {
        throw std::runtime_error("dataset directory not found: " + dir.string());
    }
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".txt") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end(),
              [](const fs::path& a, const fs::path& b) { return a.filename() < b.filename(); });
    return files;
}

std::string DocumentLoader::readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open file: " + path.string());
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        throw std::runtime_error("cannot determine size of: " + path.string());
    }
    std::string content(static_cast<std::size_t>(size), '\0');
    in.seekg(0, std::ios::beg);
    if (size > 0 && !in.read(content.data(), size)) {
        throw std::runtime_error("cannot read file: " + path.string());
    }
    return content;
}

std::string DocumentLoader::documentName(const fs::path& path) {
    return path.stem().string();
}

}  // namespace search::core
