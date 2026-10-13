#include "glm_client.hpp"

#include <map>
#include <stdexcept>

#include "httplib_config.hpp"

json GlmClient::chat(const json& messages, const json& tools, const std::function<void(const std::string&)>& on_delta) {
    if (cfg_.api_key.empty() || cfg_.api_key.rfind("请填写", 0) == 0)
        throw std::runtime_error("未配置 GLM API Key，请在 config.json 的 glm.api_key 中填写");

    json body = {{"model", cfg_.model},
                 {"messages", messages},
                 {"stream", true},
                 {"temperature", cfg_.temperature},
                 {"max_tokens", cfg_.max_tokens}};
    // 思考/推理控制：部分 GLM 推理模型不支持关闭思考，仅接受 reasoning_effort=low/high/max。
    // 约定：reasoning_effort 非空则下发该字段；否则按 thinking 开关下发 thinking.enabled；都未配置则不发，用模型默认。
    if (!cfg_.reasoning_effort.empty())
        body["reasoning_effort"] = cfg_.reasoning_effort;
    else if (cfg_.thinking)
        body["thinking"] = {{"type", "enabled"}};
    if (!tools.empty()) {
        body["tools"] = tools;
        body["tool_choice"] = "auto";
    }

    httplib::Client cli(cfg_.base_url);
    cli.set_connection_timeout(15);
    cli.set_read_timeout(cfg_.timeout_sec);
    cli.set_write_timeout(30);

    std::string buffer, raw_head, content;
    std::map<int, json> tool_calls;  // index -> {id,type,function:{name,arguments}}

    auto handle_line = [&](const std::string& line) {
        if (line.rfind("data:", 0) != 0) return;
        std::string data = line.substr(5);
        if (!data.empty() && data[0] == ' ') data.erase(0, 1);
        if (data == "[DONE]" || data.empty()) return;
        json chunk = json::parse(data, nullptr, false);
        if (chunk.is_discarded() || !chunk.contains("choices") || chunk["choices"].empty()) return;
        const json& delta = chunk["choices"][0].value("delta", json::object());
        if (delta.contains("content") && delta["content"].is_string()) {
            std::string piece = delta["content"].get<std::string>();
            if (!piece.empty()) {
                content += piece;
                on_delta(piece);
            }
        }
        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            for (const auto& tc : delta["tool_calls"]) {
                int idx = tc.value("index", static_cast<int>(tool_calls.size()));
                json& acc = tool_calls[idx];
                if (acc.is_null()) acc = {{"id", ""}, {"type", "function"}, {"function", {{"name", ""}, {"arguments", ""}}}};
                if (tc.contains("id") && tc["id"].is_string()) acc["id"] = tc["id"];
                if (tc.contains("function")) {
                    const auto& f = tc["function"];
                    if (f.contains("name") && f["name"].is_string())
                        acc["function"]["name"] = acc["function"]["name"].get<std::string>() + f["name"].get<std::string>();
                    if (f.contains("arguments")) {
                        std::string a = f["arguments"].is_string() ? f["arguments"].get<std::string>() : f["arguments"].dump();
                        acc["function"]["arguments"] = acc["function"]["arguments"].get<std::string>() + a;
                    }
                }
            }
        }
    };

    httplib::Request req;
    req.method = "POST";
    req.path = cfg_.chat_path;
    req.headers = {{"Authorization", "Bearer " + cfg_.api_key}, {"Accept", "text/event-stream"}};
    req.set_header("Content-Type", "application/json");
    req.body = body.dump();
    req.content_receiver = [&](const char* data, size_t len, uint64_t, uint64_t) {
        if (raw_head.size() < 4096) raw_head.append(data, std::min<size_t>(len, 4096 - raw_head.size()));
        buffer.append(data, len);
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buffer.erase(0, pos + 1);
            handle_line(line);
        }
        return true;
    };

    auto res = cli.send(req);
    if (!res) throw std::runtime_error("调用 GLM 接口失败: " + httplib::to_string(res.error()));
    if (res->status != 200) {
        std::string detail = raw_head.empty() ? res->body : raw_head;
        throw std::runtime_error("GLM 接口返回 HTTP " + std::to_string(res->status) + ": " + detail.substr(0, 500));
    }
    if (!buffer.empty()) handle_line(buffer);

    json msg = {{"role", "assistant"}, {"content", content}};
    if (!tool_calls.empty()) {
        msg["tool_calls"] = json::array();
        for (auto& [i, tc] : tool_calls) {
            if (tc["id"].get<std::string>().empty()) tc["id"] = "call_" + std::to_string(i);
            msg["tool_calls"].push_back(tc);
        }
    }
    return msg;
}
