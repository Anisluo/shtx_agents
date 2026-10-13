#include "product_repo.hpp"

#include <cctype>
#include <cstdio>
#include <regex>

namespace products {
namespace {

std::vector<std::string> split_keywords(const std::string& s) {
    std::vector<std::string> out;
    static const std::regex sep(R"([\s,，;；、/|]+)");
    for (std::sregex_token_iterator it(s.begin(), s.end(), sep, -1), end; it != end; ++it) {
        std::string t = *it;
        if (!t.empty()) out.push_back(t);
    }
    if (out.size() > 12) out.resize(12);
    return out;
}

}  // namespace

json to_public(const json& row, bool full) {
    json p;
    for (const char* k : {"model", "category", "series", "title", "packages"}) p[k] = row.value(k, "");
    p["key_specs"] = json::parse(row.value("key_specs", "{}"), nullptr, false);
    if (p["key_specs"].is_discarded()) p["key_specs"] = json::object();
    std::string ds = row.value("datasheet", "");
    p["datasheet"] = ds;
    p["datasheet_url"] = ds.empty() ? "" : "/datasheets/" + ds;
    if (full) {
        for (const char* k : {"description", "features", "applications"}) p[k] = row.value(k, "");
    } else {
        std::string d = row.value("description", "");
        // 摘要截断到约 120 个汉字（按 UTF-8 字符边界）
        size_t bytes = 0, chars = 0;
        while (bytes < d.size() && chars < 120) {
            unsigned char c = d[bytes];
            bytes += (c < 0x80) ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            ++chars;
        }
        p["summary"] = bytes < d.size() ? d.substr(0, bytes) + "…" : d;
    }
    return p;
}

json list_categories(Db& db) {
    json rows = db.query("SELECT category, series, COUNT(*) AS n FROM products GROUP BY category, series ORDER BY category, series");
    json out = json::array();
    for (const auto& r : rows) {
        std::string cat = r["category"];
        if (out.empty() || out.back()["category"] != cat) out.push_back({{"category", cat}, {"count", 0}, {"series", json::array()}});
        out.back()["series"].push_back({{"name", r["series"]}, {"count", r["n"]}});
        out.back()["count"] = out.back()["count"].get<long long>() + r["n"].get<long long>();
    }
    return out;
}

json search(Db& db, const std::string& keywords, const std::string& category, const std::string& series, int limit) {
    if (limit <= 0 || limit > 50) limit = 15;
    auto kws = split_keywords(keywords);
    std::string score = "0";
    std::vector<DbParam> params;
    for (const auto& k : kws) {
        // 权重：型号 5 > 系列/标题/大类 2 > 全文 1
        score += " + (CASE WHEN model LIKE ?1x THEN 5 ELSE 0 END)"
                 " + (CASE WHEN series LIKE ?1x OR title LIKE ?1x OR category LIKE ?1x THEN 2 ELSE 0 END)"
                 " + (CASE WHEN search_text LIKE ?1x THEN 1 ELSE 0 END)";
        std::string n = std::to_string(params.size() + 1);
        for (size_t pos; (pos = score.find("?1x")) != std::string::npos;) score.replace(pos, 3, "?" + n);
        params.emplace_back("%" + k + "%");
    }
    std::string sql = "SELECT *, (" + score + ") AS score FROM products WHERE 1=1";
    if (!category.empty()) {
        sql += " AND category LIKE ?";
        params.emplace_back("%" + category + "%");
    }
    if (!series.empty()) {
        sql += " AND series LIKE ?";
        params.emplace_back("%" + series + "%");
    }
    if (!kws.empty()) sql += " AND score > 0";
    sql += " ORDER BY score DESC, model LIMIT ?";
    params.emplace_back(static_cast<long long>(limit));

    json out = json::array();
    for (const auto& r : db.query(sql, params)) {
        json p = to_public(r, false);
        p["match_score"] = r["score"];
        out.push_back(std::move(p));
    }
    return out;
}

json get(Db& db, const std::string& model) {
    json r = db.query_one("SELECT * FROM products WHERE model = ?", {model});
    if (r.is_null()) r = db.query_one("SELECT * FROM products WHERE model LIKE ? ORDER BY length(model) LIMIT 1", {model + "%"});
    return r.is_null() ? r : to_public(r, true);
}

json details(Db& db, const std::vector<std::string>& models) {
    json out = json::array();
    for (const auto& m : models) {
        json p = get(db, m);
        if (p.is_null()) out.push_back({{"model", m}, {"error", "产品库中未找到该型号"}});
        else out.push_back(std::move(p));
        if (out.size() >= 8) break;
    }
    return out;
}

}  // namespace products

namespace competitors {

json to_public(Db& db, const json& row, bool with_tx) {
    json c;
    for (const char* k : {"model", "brand", "aliases", "category", "description", "note"}) c[k] = row.value(k, "");
    std::string ds = row.value("datasheet_url", "");
    c["datasheet_url"] = ds;
    // 在线搜索该竞品公开数据手册（无官方链接时的兜底：按型号到公开数据手册库检索）
    std::string q;
    for (char ch : c["model"].get<std::string>()) {
        if (std::isalnum(static_cast<unsigned char>(ch))) q += ch;
        else { char b[4]; std::snprintf(b, sizeof(b), "%%%02X", static_cast<unsigned char>(ch)); q += b; }
    }
    c["datasheet_search"] = "https://www.alldatasheet.com/view.jsp?Searchword=" + q;
    c["key_specs"] = json::parse(row.value("key_specs", "{}"), nullptr, false);
    if (c["key_specs"].is_discarded()) c["key_specs"] = json::object();
    // 太矽替代型号
    std::vector<std::string> models;
    std::string tx = row.value("tx_models", "");
    for (size_t pos = 0; pos < tx.size();) {
        size_t comma = tx.find(',', pos);
        std::string m = tx.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!m.empty()) models.push_back(m);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    c["tx_models"] = models;
    if (with_tx) {
        c["replacements"] = json::array();
        for (const auto& m : models) {
            json row2 = db.query_one("SELECT * FROM products WHERE model=?", {m});
            if (!row2.is_null()) c["replacements"].push_back(products::to_public(row2, false));
            else c["replacements"].push_back({{"model", m}});
        }
    }
    return c;
}

json search(Db& db, const std::string& keywords, int limit) {
    if (limit <= 0 || limit > 100) limit = 50;
    std::string kw;
    for (char ch : keywords) kw += (ch == ',' || ch == '，' || ch == ';') ? ' ' : ch;
    json rows;
    if (kw.find_first_not_of(' ') == std::string::npos) {
        rows = db.query("SELECT * FROM competitors ORDER BY brand, model LIMIT ?", {static_cast<long long>(limit)});
    } else {
        rows = db.query("SELECT * FROM competitors WHERE model LIKE ?1 OR brand LIKE ?1 OR aliases LIKE ?1 "
                        "OR search_text LIKE ?1 OR tx_models LIKE ?1 ORDER BY brand, model LIMIT ?2",
                        {"%" + std::string(keywords) + "%", static_cast<long long>(limit)});
    }
    json out = json::array();
    for (const auto& r : rows) out.push_back(to_public(db, r, true));
    return out;
}

json find(Db& db, const std::vector<std::string>& parts) {
    json out = json::array();
    for (const auto& raw : parts) {
        std::string p = raw;
        while (!p.empty() && (p.front() == ' ')) p.erase(p.begin());
        while (!p.empty() && (p.back() == ' ')) p.pop_back();
        if (p.empty()) continue;
        json r = db.query_one("SELECT * FROM competitors WHERE model = ?", {p});
        if (r.is_null())
            r = db.query_one("SELECT * FROM competitors WHERE model LIKE ?1 OR aliases LIKE ?1 ORDER BY length(model) LIMIT 1",
                             {"%" + p + "%"});
        if (r.is_null()) out.push_back({{"query", p}, {"found", false}, {"hint", "竞品库暂无该料号的交叉对照"}});
        else {
            json c = to_public(db, r, true);
            c["query"] = p;
            c["found"] = true;
            out.push_back(std::move(c));
        }
        if (out.size() >= 8) break;
    }
    return out;
}

json for_tx_model(Db& db, const std::string& model) {
    json rows = db.query("SELECT * FROM competitors WHERE ','||replace(tx_models,' ','')||',' LIKE ?",
                         {"%," + model + ",%"});
    json out = json::array();
    for (const auto& r : rows)
        out.push_back({{"model", r.value("model", "")}, {"brand", r.value("brand", "")}, {"note", r.value("note", "")}});
    return out;
}

}  // namespace competitors
