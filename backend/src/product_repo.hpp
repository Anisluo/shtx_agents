#pragma once
// 产品库查询（供 REST 接口与 Agent 工具共用）
#include <string>
#include <vector>

#include "db.hpp"

namespace products {

json list_categories(Db& db);
// keywords 以空格/逗号分隔，按命中关键词数打分；category/series 可选精确过滤（模糊匹配）
json search(Db& db, const std::string& keywords, const std::string& category, const std::string& series, int limit,
            const json& filters = json::object());
json details(Db& db, const std::vector<std::string>& models);
json get(Db& db, const std::string& model);
// 行 -> API 输出（解析 key_specs，补充 datasheet_url）
json to_public(const json& row, bool full);

}  // namespace products

namespace competitors {

json to_public(Db& db, const json& row, bool with_tx);
json search(Db& db, const std::string& keywords, int limit);
// 按竞品/标准料号查找太矽替代（parts 可含型号或别名）
json find(Db& db, const std::vector<std::string>& parts);
// 某个太矽型号可替代的竞品（反查，用于产品详情页）
json for_tx_model(Db& db, const std::string& model);

}  // namespace competitors
