#pragma once
// 轻量 SQLite 封装：每个请求打开一个连接，参数化查询，结果直接转 JSON。
#include <sqlite3.h>

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using json = nlohmann::json;
using DbParam = std::variant<std::nullptr_t, long long, double, std::string>;

class Db {
public:
    explicit Db(const std::string& path);
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    // SELECT：返回对象数组
    json query(const std::string& sql, const std::vector<DbParam>& params = {});
    // 单行查询，没有结果时返回 null
    json query_one(const std::string& sql, const std::vector<DbParam>& params = {});
    // INSERT/UPDATE/DELETE：返回受影响行数
    int exec(const std::string& sql, const std::vector<DbParam>& params = {});
    void exec_script(const std::string& sql);
    long long last_insert_id() const;

private:
    sqlite3* h_ = nullptr;
    sqlite3_stmt* prepare(const std::string& sql, const std::vector<DbParam>& params);
};

void init_schema(const std::string& path);
