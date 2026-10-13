#pragma once
// 产品推荐 Agent：GLM 工具调用循环，工具直接查询 SQLite 产品库
#include <functional>
#include <string>

#include "glm_client.hpp"

struct AgentResult {
    std::string content;   // 最终回复（Markdown）
    json steps = json::array();     // 工具调用记录
    json products = json::array();  // 回复中推荐/提到的产品卡片
};

class Agent {
public:
    Agent(GlmConfig cfg, std::string db_path, int max_rounds = 6)
        : client_(std::move(cfg)), db_path_(std::move(db_path)), max_rounds_(max_rounds) {}

    // history：之前的 user/assistant 消息（不含 system）；emit 推送流式事件
    AgentResult run(const json& history, const std::function<void(const json&)>& emit);

    static json tool_schemas();

private:
    json call_tool(const std::string& name, const json& args, std::string& summary);

    GlmClient client_;
    std::string db_path_;
    int max_rounds_;
};
