#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <string>

#include "ollama/LlmClient.hpp"

namespace search::test {

/// Unique temporary directory removed on destruction.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("search_test_" + std::to_string(rd()) + "_" + std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    std::filesystem::path write(const std::string& relative, const std::string& content) const {
        const auto file = path_ / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out << content;
        return file;
    }

private:
    static inline std::atomic<int> counter_{0};
    std::filesystem::path path_;
};

/// Scriptable LlmClient used instead of Ollama in unit tests.
class FakeLlmClient : public ollama::LlmClient {
public:
    std::atomic<bool> available{true};
    std::atomic<int> calls{0};

    void setResponse(std::optional<std::string> response, std::string error = {}) {
        std::lock_guard lock(mutex_);
        response_ = std::move(response);
        error_ = std::move(error);
    }

    bool isAvailable() override { return available.load(); }

    std::optional<std::string> generate(const std::string& prompt, std::string* error) override {
        ++calls;
        std::lock_guard lock(mutex_);
        lastPrompt_ = prompt;
        if (!response_ && error != nullptr) {
            *error = error_;
        }
        return response_;
    }

    [[nodiscard]] std::string model() const override { return "fake-model"; }

    std::string lastPrompt() {
        std::lock_guard lock(mutex_);
        return lastPrompt_;
    }

private:
    std::mutex mutex_;
    std::optional<std::string> response_ = std::string(R"({"original":"q","expanded_terms":[]})");
    std::string error_;
    std::string lastPrompt_;
};

}  // namespace search::test
