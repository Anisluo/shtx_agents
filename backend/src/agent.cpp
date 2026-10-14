#include "agent.hpp"

#include <algorithm>
#include <set>

#include "db.hpp"
#include "product_repo.hpp"

namespace {

const char* kSystemPrompt = R"(你是「上海太矽电子科技有限公司」(TXSemi，官网 txsemi.com) 的资深 FAE（现场应用工程师）与产品选型助手。
太矽电子主营高性能模拟及数模混合 IC：电源管理（LDO、DC-DC、电压检测、复位、基准、锂电充电）、霍尔传感器、驱动芯片、MCU、通信协议芯片、红外热释/雷达处理、电子烟芯片、定时器芯片等。

你的任务：理解用户的应用场景与电气需求，从太矽电子产品库中推荐最合适的型号。

工作规则：
1. 必须先使用工具查询产品库，只能推荐产品库中真实存在的型号，严禁编造型号或参数。
2. 选型常用流程：list_categories 了解产品线 → search_products 检索候选（可只按 category/series 列出整个系列做参数比对）→ get_product_details 查看关键参数、特性与应用。
   当用户提到其它厂商的型号或标准料号（如 AMS1117、LM317、TL431、NE555、XC6206、ULN2003、MAX485、A3144、TP4054、7805、MAX809 等）时，先用 find_alternative 查找太矽的国产替代型号，再用 get_product_details 核对替代型号参数是否满足需求。
3. 关键参数要逐项核对用户需求：输入/工作电压、极限耐压、输出电压、输出电流、静态功耗、精度、压差、PSRR、封装、温度范围等。参数不满足时要明确说明。
4. 若用户需求信息不足（例如未给出输入电压范围、负载电流、封装要求），先给出初步推荐，再列出需要补充确认的关键问题。
5. 回答使用简体中文 Markdown，结构建议：
   - **需求理解**：一句话总结
   - **推荐型号**：用表格列出 1~3 个型号（型号 | 关键参数 | 封装 | 推荐理由），首选放第一行
   - **选型说明 / 设计注意事项**：外围器件、散热、布局等实用建议
   - **数据手册**：以 Markdown 链接给出 datasheet_url，例如 [TX6206 数据手册](/datasheets/TX6206_V2.2.pdf)
6. 用户可能上传附件（以「〔以下为用户上传的附件，供分析参考〕」标注，含 datasheet、需求规格书、BOM 等）。请据此提炼关键参数/需求：若是竞品 datasheet，用 find_alternative 找国产替代并核对参数；若是需求文档，据此检索推荐。附件为自动提取文本，可能存在排版错乱，请结合常识判断。
7. 如果产品库中确实没有合适型号，坦诚说明，并建议联系太矽电子销售/FAE：021-51872639，jwu@txsemi.com。
8. 与芯片选型无关的问题，礼貌地引导回产品咨询。)";

std::string trim_json_for_model(const json& j) {
    std::string s = j.dump();
    if (s.size() > 24000) s = s.substr(0, 24000) + "...(结果过长已截断，请缩小检索范围)";
    return s;
}

}  // namespace

json Agent::tool_schemas() {
    return json::array({
        {{"type", "function"},
         {"function",
          {{"name", "list_categories"},
           {"description", "列出太矽电子产品库的全部产品大类、系列及型号数量。用于了解产品线。"},
           {"parameters", {{"type", "object"}, {"properties", json::object()}}}}}},
        {{"type", "function"},
         {"function",
          {{"name", "search_products"},
           {"description",
            "按关键词检索产品，返回候选型号及关键参数摘要。关键词用空格分隔，建议拆成短词，例如 \"LDO 低功耗 SOT23\"、\"霍尔 风扇\"、\"降压 Buck 2A\"、\"TX6206\"。"
            "也可以不给关键词，仅用 category 或 series 列出整个系列做参数比对。"},
           {"parameters",
            {{"type", "object"},
             {"properties",
              {{"keywords", {{"type", "string"}, {"description", "空格分隔的关键词，可为空"}}},
               {"category", {{"type", "string"}, {"description", "产品大类过滤（可选），如 电源管理芯片、霍尔传感器芯片"}}},
               {"series", {{"type", "string"}, {"description", "系列过滤（可选），如 LDO、Boost、复位"}}},
               {"limit", {{"type", "integer"}, {"description", "返回数量上限，默认 15，最大 50"}}}}}}}}}},
        {{"type", "function"},
         {"function",
          {{"name", "find_alternative"},
           {"description",
            "国产替代查询：当用户提到其它品牌型号或行业标准料号（如 AMS1117、LM317、TL431、NE555、XC6206、ULN2003、ULN2803、MAX485、SP3485、A3144、TP4054、7805、78L05、MAX809 等）时，"
            "查找太矽电子对应的替代型号。返回竞品信息及太矽替代型号（含关键参数）。"},
           {"parameters",
            {{"type", "object"},
             {"properties", {{"parts", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "竞品/标准料号列表，最多 8 个"}}}}},
             {"required", {"parts"}}}}}}},
        {{"type", "function"},
         {"function",
          {{"name", "get_product_details"},
           {"description", "获取指定型号的完整资料：概述、特性、应用、关键参数、封装、数据手册链接。"},
           {"parameters",
            {{"type", "object"},
             {"properties", {{"models", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "型号列表，最多 8 个"}}}}},
             {"required", {"models"}}}}}}},
    });
}

json Agent::call_tool(const std::string& name, const json& args, std::string& summary) {
    Db db(db_path_);
    if (name == "list_categories") {
        json r = products::list_categories(db);
        summary = "共 " + std::to_string(r.size()) + " 个产品大类";
        return r;
    }
    if (name == "search_products") {
        json r = products::search(db, args.value("keywords", ""), args.value("category", ""), args.value("series", ""),
                                  args.value("limit", 15));
        summary = "找到 " + std::to_string(r.size()) + " 个候选型号";
        return r;
    }
    if (name == "find_alternative") {
        std::vector<std::string> parts;
        if (args.contains("parts") && args["parts"].is_array())
            for (const auto& m : args["parts"])
                if (m.is_string()) parts.push_back(m.get<std::string>());
        json r = competitors::find(db, parts);
        int hit = 0;
        for (const auto& c : r) hit += c.value("found", false) ? 1 : 0;
        summary = "交叉对照 " + std::to_string(parts.size()) + " 个料号，命中 " + std::to_string(hit) + " 个";
        return r;
    }
    if (name == "get_product_details") {
        std::vector<std::string> models;
        if (args.contains("models") && args["models"].is_array())
            for (const auto& m : args["models"])
                if (m.is_string()) models.push_back(m.get<std::string>());
        json r = products::details(db, models);
        summary = "查看 " + std::to_string(r.size()) + " 个型号的详细资料";
        return r;
    }
    summary = "未知工具";
    return {{"error", "未知工具: " + name}};
}

AgentResult Agent::run(const json& history, const std::function<void(const json&)>& emit) {
    json messages = json::array({{{"role", "system"}, {"content", kSystemPrompt}}});
    for (const auto& m : history) messages.push_back(m);

    AgentResult result;
    json product_pool = json::object();

    for (int round = 0; round < max_rounds_; ++round) {
        // 最后一轮不再提供工具，强制模型给出结论
        json tools = (round == max_rounds_ - 1) ? json::array() : tool_schemas();
        json msg = client_.chat(messages, tools, [&](const std::string& piece) {
            result.content += piece;
            emit({{"type", "delta"}, {"text", piece}});
        });
        messages.push_back(msg);

        if (!msg.contains("tool_calls")) break;
        if (!result.content.empty() && result.content.back() != '\n') {
            result.content += "\n\n";
            emit({{"type", "delta"}, {"text", "\n\n"}});
        }
        for (const auto& tc : msg["tool_calls"]) {
            std::string name = tc["function"]["name"];
            json args = json::parse(tc["function"].value("arguments", "{}"), nullptr, false);
            if (args.is_discarded() || !args.is_object()) args = json::object();
            emit({{"type", "step"}, {"tool", name}, {"args", args}, {"status", "running"}});

            std::string summary;
            json out;
            try {
                out = call_tool(name, args, summary);
            } catch (const std::exception& e) {
                out = {{"error", e.what()}};
                summary = std::string("出错: ") + e.what();
            }
            // 记录候选产品，用于最终产品卡片
            if (out.is_array())
                for (const auto& p : out)
                    if (p.is_object() && p.contains("model") && !p.contains("error")) product_pool[p["model"].get<std::string>()] = p;

            json step = {{"tool", name}, {"args", args}, {"summary", summary}};
            result.steps.push_back(step);
            emit({{"type", "step"}, {"tool", name}, {"args", args}, {"status", "done"}, {"summary", summary}});
            messages.push_back({{"role", "tool"}, {"tool_call_id", tc["id"]}, {"content", trim_json_for_model(out)}});
        }
    }

    // 最终回复中提到的型号 -> 产品卡片（保持在回复中出现的顺序）
    std::vector<std::pair<size_t, std::string>> hits;
    for (auto& [model, p] : product_pool.items()) {
        size_t pos = result.content.find(model);
        if (pos != std::string::npos) hits.emplace_back(pos, model);
    }
    std::sort(hits.begin(), hits.end());
    Db db(db_path_);
    for (const auto& [pos, model] : hits) {
        json row = db.query_one("SELECT * FROM products WHERE model=?", {model});
        if (!row.is_null()) result.products.push_back(products::to_public(row, false));
        if (result.products.size() >= 6) break;
    }
    return result;
}
