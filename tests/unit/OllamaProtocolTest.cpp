#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "ollama/OllamaProtocol.hpp"

using search::ollama::OllamaProtocol;

TEST(OllamaProtocolTest, GenerateRequestIsNonStreamingJsonMode) {
    const auto body = nlohmann::json::parse(OllamaProtocol::buildGenerateRequest("qwen2.5:3b", "hello", 64, 0.1));
    EXPECT_EQ(body["model"], "qwen2.5:3b");
    EXPECT_EQ(body["prompt"], "hello");
    EXPECT_EQ(body["stream"], false);
    EXPECT_EQ(body["format"], "json");
    EXPECT_EQ(body["options"]["num_predict"], 64);
    EXPECT_DOUBLE_EQ(body["options"]["temperature"].get<double>(), 0.1);
}

TEST(OllamaProtocolTest, ExtractsResponseText) {
    const auto text = OllamaProtocol::extractResponseText(R"({"model":"m","response":"{\"a\":1}","done":true})");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, R"({"a":1})");
}

TEST(OllamaProtocolTest, ReportsErrors) {
    std::string error;
    EXPECT_FALSE(OllamaProtocol::extractResponseText(R"({"error":"model 'x' not found"})", &error).has_value());
    EXPECT_NE(error.find("not found"), std::string::npos);
    EXPECT_FALSE(OllamaProtocol::extractResponseText("<html>bad gateway</html>", &error).has_value());
    EXPECT_FALSE(OllamaProtocol::extractResponseText(R"({"response":42})", &error).has_value());
    EXPECT_FALSE(OllamaProtocol::extractResponseText("", &error).has_value());
}

TEST(OllamaProtocolTest, ModelListed) {
    const std::string tags = R"({"models":[{"name":"qwen2.5:3b","model":"qwen2.5:3b"},{"name":"llama3:latest"}]})";
    EXPECT_TRUE(OllamaProtocol::modelListed(tags, "qwen2.5:3b"));
    EXPECT_TRUE(OllamaProtocol::modelListed(tags, "llama3"));
    EXPECT_FALSE(OllamaProtocol::modelListed(tags, "qwen2.5:7b"));
    EXPECT_FALSE(OllamaProtocol::modelListed(R"({"models":[]})", "qwen2.5:3b"));
    EXPECT_FALSE(OllamaProtocol::modelListed("garbage", "qwen2.5:3b"));
}
