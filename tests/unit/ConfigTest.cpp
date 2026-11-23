#include <gtest/gtest.h>

#include <cstdlib>

#include "core/Config.hpp"

using search::core::Config;

namespace {

/// Sets an environment variable for the lifetime of the object.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) { ::setenv(name, value, 1); }
    ~ScopedEnv() { ::unsetenv(name_); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
};

}  // namespace

TEST(ConfigTest, DefaultsWithoutEnvironment) {
    const Config cfg = Config::fromEnvironment();
    EXPECT_EQ(cfg.port, 8080);
    EXPECT_EQ(cfg.requestThreads, 16u);
    EXPECT_EQ(cfg.llmThreads, 4u);
    EXPECT_TRUE(cfg.warnings.empty());
}

TEST(ConfigTest, ValidValuesAreApplied) {
    ScopedEnv a("REQUEST_THREADS", "8");
    ScopedEnv b("SERVER_PORT", "9090");
    ScopedEnv c("OLLAMA_TIMEOUT_SEC", "12.5");
    ScopedEnv d("LLM_THREADS", "2");
    const Config cfg = Config::fromEnvironment();
    EXPECT_EQ(cfg.requestThreads, 8u);
    EXPECT_EQ(cfg.port, 9090);
    EXPECT_DOUBLE_EQ(cfg.ollamaTimeoutSec, 12.5);
    EXPECT_EQ(cfg.llmThreads, 2u);
    EXPECT_TRUE(cfg.warnings.empty());
}

TEST(ConfigTest, NegativeValueKeepsDefaultAndWarns) {
    ScopedEnv a("REQUEST_THREADS", "-1");
    const Config cfg = Config::fromEnvironment();
    EXPECT_EQ(cfg.requestThreads, 16u);  // previously wrapped to 2^64-1 and crashed the server
    ASSERT_EQ(cfg.warnings.size(), 1u);
    EXPECT_NE(cfg.warnings[0].find("REQUEST_THREADS"), std::string::npos);
}

TEST(ConfigTest, MalformedAndOutOfRangeValuesAreRejected) {
    ScopedEnv a("IO_THREADS", "abc");
    ScopedEnv b("SERVER_PORT", "70000");
    ScopedEnv c("INDEX_THREADS", "12abc");
    ScopedEnv d("OLLAMA_TIMEOUT_SEC", "-5");
    ScopedEnv e("MAX_LIMIT", "0");
    const Config cfg = Config::fromEnvironment();
    EXPECT_EQ(cfg.ioThreads, 4u);
    EXPECT_EQ(cfg.port, 8080);
    EXPECT_DOUBLE_EQ(cfg.ollamaTimeoutSec, 30.0);
    EXPECT_EQ(cfg.maxLimit, 100u);
    EXPECT_EQ(cfg.warnings.size(), 5u);
}
