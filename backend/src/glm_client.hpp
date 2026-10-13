#pragma once
// 智谱 GLM (OpenAI 兼容) Chat Completions 客户端：流式输出 + 工具调用
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct GlmConfig {
    std::string api_key;
    std::string base_url = "https://open.bigmodel.cn";
    std::string chat_path = "/api/paas/v4/chat/completions";
    std::string model = "glm-5.3";
    double temperature = 0.3;
    int max_tokens = 4096;
    int timeout_sec = 120;
    bool thinking = false;
    // 推理强度：空=不发送该字段（用模型默认）；可选 "low" / "high" / "max"
    std::string reasoning_effort;
};

class GlmClient {
public:
    explicit GlmClient(GlmConfig cfg) : cfg_(std::move(cfg)) {}

    // 发送一次对话请求（stream=true）。on_delta 收到正文增量。
    // 返回完整的 assistant message：{role, content, tool_calls?}
    json chat(const json& messages, const json& tools, const std::function<void(const std::string&)>& on_delta);

    const GlmConfig& config() const { return cfg_; }

private:
    GlmConfig cfg_;
};
