#include "db.hpp"

Db::Db(const std::string& path) {
    if (sqlite3_open_v2(path.c_str(), &h_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        std::string msg = h_ ? sqlite3_errmsg(h_) : "unknown";
        sqlite3_close(h_);
        throw std::runtime_error("无法打开数据库 " + path + ": " + msg);
    }
    sqlite3_busy_timeout(h_, 5000);
    exec_script("PRAGMA foreign_keys=ON;");
}

Db::~Db() { sqlite3_close(h_); }

sqlite3_stmt* Db::prepare(const std::string& sql, const std::vector<DbParam>& params) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(h_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("SQL 错误: ") + sqlite3_errmsg(h_));
    for (size_t i = 0; i < params.size(); ++i) {
        int idx = static_cast<int>(i + 1);
        const auto& p = params[i];
        if (std::holds_alternative<std::nullptr_t>(p)) sqlite3_bind_null(st, idx);
        else if (auto v = std::get_if<long long>(&p)) sqlite3_bind_int64(st, idx, *v);
        else if (auto d = std::get_if<double>(&p)) sqlite3_bind_double(st, idx, *d);
        else {
            const auto& s = std::get<std::string>(p);
            sqlite3_bind_text(st, idx, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
        }
    }
    return st;
}

json Db::query(const std::string& sql, const std::vector<DbParam>& params) {
    sqlite3_stmt* st = prepare(sql, params);
    json rows = json::array();
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        json row = json::object();
        int n = sqlite3_column_count(st);
        for (int c = 0; c < n; ++c) {
            const char* name = sqlite3_column_name(st, c);
            switch (sqlite3_column_type(st, c)) {
                case SQLITE_INTEGER: row[name] = sqlite3_column_int64(st, c); break;
                case SQLITE_FLOAT: row[name] = sqlite3_column_double(st, c); break;
                case SQLITE_NULL: row[name] = nullptr; break;
                default:
                    row[name] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, c)),
                                            sqlite3_column_bytes(st, c));
            }
        }
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) throw std::runtime_error(std::string("SQL 执行失败: ") + sqlite3_errmsg(h_));
    return rows;
}

json Db::query_one(const std::string& sql, const std::vector<DbParam>& params) {
    json rows = query(sql, params);
    return rows.empty() ? json(nullptr) : rows[0];
}

int Db::exec(const std::string& sql, const std::vector<DbParam>& params) {
    sqlite3_stmt* st = prepare(sql, params);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error(std::string("SQL 执行失败: ") + sqlite3_errmsg(h_));
    return sqlite3_changes(h_);
}

void Db::exec_script(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(h_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("SQL 脚本失败: " + msg);
    }
}

long long Db::last_insert_id() const { return sqlite3_last_insert_rowid(h_); }

// 与 tools/build_db.py 中的建表语句保持一致（IF NOT EXISTS，可重复执行）
void init_schema(const std::string& path) {
    Db db(path);
    db.exec_script(R"SQL(
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL UNIQUE COLLATE NOCASE,
    name TEXT NOT NULL DEFAULT '',
    dept TEXT NOT NULL DEFAULT '',
    role TEXT NOT NULL DEFAULT 'user',
    pw_salt TEXT NOT NULL,
    pw_hash TEXT NOT NULL,
    pw_iter INTEGER NOT NULL DEFAULT 120000,
    active INTEGER NOT NULL DEFAULT 1,
    created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
    last_login TEXT
);
CREATE TABLE IF NOT EXISTS sessions (
    token TEXT PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
    expires_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS competitors (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    model TEXT NOT NULL UNIQUE COLLATE NOCASE,
    brand TEXT NOT NULL DEFAULT '',
    aliases TEXT NOT NULL DEFAULT '',
    category TEXT NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '',
    key_specs TEXT NOT NULL DEFAULT '{}',
    tx_models TEXT NOT NULL DEFAULT '',
    datasheet_url TEXT NOT NULL DEFAULT '',
    note TEXT NOT NULL DEFAULT '',
    search_text TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS products (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    model TEXT NOT NULL UNIQUE COLLATE NOCASE,
    category TEXT NOT NULL DEFAULT '',
    series TEXT NOT NULL DEFAULT '',
    title TEXT NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '',
    features TEXT NOT NULL DEFAULT '',
    applications TEXT NOT NULL DEFAULT '',
    packages TEXT NOT NULL DEFAULT '',
    key_specs TEXT NOT NULL DEFAULT '{}',
    datasheet TEXT NOT NULL DEFAULT '',
    search_text TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS conversations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title TEXT NOT NULL DEFAULT '新对话',
    created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
    updated_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
);
CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    role TEXT NOT NULL,
    content TEXT NOT NULL,
    meta TEXT NOT NULL DEFAULT '{}',
    created_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
);
CREATE INDEX IF NOT EXISTS idx_msg_conv ON messages(conversation_id, id);
CREATE INDEX IF NOT EXISTS idx_conv_user ON conversations(user_id, updated_at);
)SQL");
}
