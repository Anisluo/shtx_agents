// 太矽电子 AI 产品推荐系统 —— C++ 后端 (REST API + SSE 流式对话)
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>

#include "agent.hpp"
#include "auth.hpp"
#include "db.hpp"
#include "httplib_config.hpp"
#include "product_repo.hpp"

namespace fs = std::filesystem;

struct AppConfig {
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string db_path, datasheet_dir, log_dir;
    int history_messages = 20;
    GlmConfig glm;
};

static AppConfig g_cfg;
static std::mutex g_log_mu;

// ---------------------------------------------------------------- 工具函数
static std::string now_str() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return os.str();
}

static void log_line(const std::string& file, const std::string& line) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    std::ofstream(fs::u8path(g_cfg.log_dir) / file, std::ios::app) << line << "\n";
}

static void log_info(const std::string& msg) {
    std::string line = "[" + now_str() + "] " + msg;
    std::cout << line << std::endl;
    log_line("backend.log", line);
}

static void reply(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

static void fail(httplib::Response& res, int status, const std::string& msg) { reply(res, {{"error", msg}}, status); }

static json body_json(const httplib::Request& req) {
    json j = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    return (j.is_discarded() || !j.is_object()) ? json::object() : j;
}

static std::string bearer(const httplib::Request& req) {
    std::string h = req.get_header_value("Authorization");
    return h.rfind("Bearer ", 0) == 0 ? h.substr(7) : "";
}

static std::optional<json> require_user(Db& db, const httplib::Request& req, httplib::Response& res) {
    auto u = auth::user_from_token(db, bearer(req));
    if (!u) fail(res, 401, "未登录或登录已过期");
    return u;
}

static bool is_admin(const json& u) { return u.value("role", "") == "admin"; }
static bool can_view_logs(const json& u) { return is_admin(u) || u.value("role", "") == "manager"; }

static std::string utf8_prefix(const std::string& s, size_t max_chars) {
    size_t bytes = 0, chars = 0;
    while (bytes < s.size() && chars < max_chars) {
        unsigned char c = s[bytes];
        bytes += (c < 0x80) ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        ++chars;
    }
    return s.substr(0, std::min(bytes, s.size()));
}

static std::string sse(const json& ev) { return "data: " + ev.dump() + "\n\n"; }

// ---------------------------------------------------------------- 配置
static void load_config(const std::string& path) {
    std::ifstream f(fs::u8path(path));
    if (!f) throw std::runtime_error("找不到配置文件: " + path);
    json c = json::parse(f, nullptr, true, true);  // 允许注释
    fs::path base = fs::absolute(fs::u8path(path)).parent_path();
    auto resolve = [&](const std::string& p) { return (base / fs::u8path(p)).lexically_normal().u8string(); };

    const json& b = c.value("backend", json::object());
    g_cfg.host = b.value("host", g_cfg.host);
    g_cfg.port = b.value("port", g_cfg.port);
    g_cfg.db_path = resolve(b.value("db_path", "data/shtx.db"));
    g_cfg.datasheet_dir = resolve(b.value("datasheet_dir", "datasheets"));
    g_cfg.log_dir = resolve(b.value("log_dir", "logs"));
    g_cfg.history_messages = b.value("history_messages", 20);

    const json& g = c.value("glm", json::object());
    g_cfg.glm.api_key = g.value("api_key", "");
    g_cfg.glm.base_url = g.value("base_url", g_cfg.glm.base_url);
    g_cfg.glm.chat_path = g.value("chat_path", g_cfg.glm.chat_path);
    g_cfg.glm.model = g.value("model", g_cfg.glm.model);
    g_cfg.glm.temperature = g.value("temperature", g_cfg.glm.temperature);
    g_cfg.glm.max_tokens = g.value("max_tokens", g_cfg.glm.max_tokens);
    g_cfg.glm.timeout_sec = g.value("timeout_sec", g_cfg.glm.timeout_sec);
    g_cfg.glm.thinking = g.value("thinking", g_cfg.glm.thinking);
    g_cfg.glm.reasoning_effort = g.value("reasoning_effort", g_cfg.glm.reasoning_effort);
    if (const char* env = std::getenv("GLM_API_KEY"); env && *env) g_cfg.glm.api_key = env;
}

// ---------------------------------------------------------------- 路由
static void register_auth_routes(httplib::Server& svr) {
    svr.Post("/api/auth/login", [](const httplib::Request& req, httplib::Response& res) {
        json b = body_json(req);
        std::string username = b.value("username", ""), password = b.value("password", "");
        Db db(g_cfg.db_path);
        json u = db.query_one("SELECT * FROM users WHERE username = ?", {username});
        if (u.is_null() || !auth::verify_password(password, u)) {
            log_info("登录失败 username=" + username + " ip=" + req.remote_addr);
            return fail(res, 401, "用户名或密码错误");
        }
        if (u["active"].get<long long>() == 0) return fail(res, 403, "账户已被停用，请联系管理员");
        std::string token = auth::create_session(db, u["id"].get<long long>());
        log_info("登录成功 user=" + u["username"].get<std::string>() + " ip=" + req.remote_addr);
        reply(res, {{"token", token}, {"user", auth::public_user(u)}});
    });

    svr.Post("/api/auth/logout", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        auth::delete_session(db, bearer(req));
        reply(res, {{"ok", true}});
    });

    svr.Get("/api/auth/me", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        if (auto u = require_user(db, req, res)) reply(res, {{"user", auth::public_user(*u)}});
    });

    svr.Post("/api/auth/password", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        auto u = require_user(db, req, res);
        if (!u) return;
        json b = body_json(req);
        std::string oldp = b.value("old_password", ""), newp = b.value("new_password", "");
        json row = db.query_one("SELECT * FROM users WHERE id=?", {(*u)["id"].get<long long>()});
        if (!auth::verify_password(oldp, row)) return fail(res, 400, "原密码不正确");
        if (newp.size() < 6) return fail(res, 400, "新密码至少 6 位");
        auto [salt, hash] = auth::make_password(newp);
        db.exec("UPDATE users SET pw_salt=?, pw_hash=?, pw_iter=? WHERE id=?",
                {salt, hash, static_cast<long long>(auth::kPbkdf2Iter), row["id"].get<long long>()});
        db.exec("DELETE FROM sessions WHERE user_id=? AND token<>?", {row["id"].get<long long>(), bearer(req)});
        log_info("修改密码 user=" + row["username"].get<std::string>());
        reply(res, {{"ok", true}});
    });
}

static void register_admin_routes(httplib::Server& svr) {
    svr.Get("/api/admin/users", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        auto u = require_user(db, req, res);
        if (!u) return;
        if (!is_admin(*u)) return fail(res, 403, "需要管理员权限");
        reply(res, {{"users", db.query("SELECT u.id, u.username, u.name, u.dept, u.role, u.active, u.created_at, u.last_login, "
                                       "(SELECT COUNT(*) FROM conversations c WHERE c.user_id=u.id) AS conversations "
                                       "FROM users u ORDER BY u.id")}});
    });

    svr.Post("/api/admin/users", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        auto u = require_user(db, req, res);
        if (!u) return;
        if (!is_admin(*u)) return fail(res, 403, "需要管理员权限");
        json b = body_json(req);
        std::string username = b.value("username", ""), password = b.value("password", "");
        if (username.empty() || password.size() < 6) return fail(res, 400, "用户名不能为空，密码至少 6 位");
        if (!db.query_one("SELECT id FROM users WHERE username=?", {username}).is_null()) return fail(res, 409, "用户名已存在");
        auto [salt, hash] = auth::make_password(password);
        db.exec("INSERT INTO users(username, name, dept, role, pw_salt, pw_hash, pw_iter) VALUES(?,?,?,?,?,?,?)",
                {username, b.value("name", ""), b.value("dept", ""), b.value("role", "user"), salt, hash,
                 static_cast<long long>(auth::kPbkdf2Iter)});
        log_info("新增用户 " + username + " by " + (*u)["username"].get<std::string>());
        reply(res, {{"id", db.last_insert_id()}}, 201);
    });

    svr.Patch(R"(/api/admin/users/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        auto u = require_user(db, req, res);
        if (!u) return;
        if (!is_admin(*u)) return fail(res, 403, "需要管理员权限");
        long long id = std::stoll(req.matches[1]);
        json target = db.query_one("SELECT * FROM users WHERE id=?", {id});
        if (target.is_null()) return fail(res, 404, "用户不存在");
        json b = body_json(req);
        bool self = id == (*u)["id"].get<long long>();
        if (self && ((b.contains("active") && !b["active"].get<bool>()) || (b.contains("role") && b["role"] != "admin")))
            return fail(res, 400, "不能停用自己或取消自己的管理员权限");
        for (const char* k : {"name", "dept", "role"})
            if (b.contains(k) && b[k].is_string()) db.exec(std::string("UPDATE users SET ") + k + "=? WHERE id=?", {b[k].get<std::string>(), id});
        if (b.contains("active")) {
            db.exec("UPDATE users SET active=? WHERE id=?", {static_cast<long long>(b["active"].get<bool>() ? 1 : 0), id});
            if (!b["active"].get<bool>()) db.exec("DELETE FROM sessions WHERE user_id=?", {id});
        }
        if (b.contains("password") && b["password"].is_string() && !b["password"].get<std::string>().empty()) {
            std::string p = b["password"];
            if (p.size() < 6) return fail(res, 400, "密码至少 6 位");
            auto [salt, hash] = auth::make_password(p);
            db.exec("UPDATE users SET pw_salt=?, pw_hash=?, pw_iter=? WHERE id=?", {salt, hash, static_cast<long long>(auth::kPbkdf2Iter), id});
            db.exec("DELETE FROM sessions WHERE user_id=?", {id});
        }
        log_info("修改用户 id=" + std::to_string(id) + " by " + (*u)["username"].get<std::string>());
        reply(res, {{"ok", true}});
    });

    svr.Delete(R"(/api/admin/users/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        auto u = require_user(db, req, res);
        if (!u) return;
        if (!is_admin(*u)) return fail(res, 403, "需要管理员权限");
        long long id = std::stoll(req.matches[1]);
        if (id == (*u)["id"].get<long long>()) return fail(res, 400, "不能删除自己");
        db.exec("DELETE FROM users WHERE id=?", {id});
        log_info("删除用户 id=" + std::to_string(id) + " by " + (*u)["username"].get<std::string>());
        reply(res, {{"ok", true}});
    });

    // 对话日志（admin / manager）
    svr.Get("/api/admin/logs", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        auto u = require_user(db, req, res);
        if (!u) return;
        if (!can_view_logs(*u)) return fail(res, 403, "无权查看对话日志");
        std::string q = req.get_param_value("q"), user = req.get_param_value("user");
        int limit = req.has_param("limit") ? std::stoi(req.get_param_value("limit")) : 200;
        std::string sql =
            "SELECT c.id, c.title, c.created_at, c.updated_at, u.username, u.name, u.dept, "
            "(SELECT COUNT(*) FROM messages m WHERE m.conversation_id=c.id) AS message_count "
            "FROM conversations c JOIN users u ON u.id=c.user_id WHERE 1=1";
        std::vector<DbParam> p;
        if (!user.empty()) {
            sql += " AND (u.username LIKE ? OR u.name LIKE ?)";
            p.emplace_back("%" + user + "%");
            p.emplace_back("%" + user + "%");
        }
        if (!q.empty()) {
            sql += " AND (c.title LIKE ? OR EXISTS (SELECT 1 FROM messages m WHERE m.conversation_id=c.id AND m.content LIKE ?))";
            p.emplace_back("%" + q + "%");
            p.emplace_back("%" + q + "%");
        }
        sql += " ORDER BY c.updated_at DESC LIMIT ?";
        p.emplace_back(static_cast<long long>(std::clamp(limit, 1, 1000)));
        json stats = db.query_one(
            "SELECT (SELECT COUNT(*) FROM users) AS users, (SELECT COUNT(*) FROM conversations) AS conversations, "
            "(SELECT COUNT(*) FROM messages WHERE role='user') AS questions, (SELECT COUNT(*) FROM products) AS products");
        reply(res, {{"conversations", db.query(sql, p)}, {"stats", stats}});
    });

    svr.Get(R"(/api/admin/conversations/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        auto u = require_user(db, req, res);
        if (!u) return;
        if (!can_view_logs(*u)) return fail(res, 403, "无权查看对话日志");
        long long id = std::stoll(req.matches[1]);
        json c = db.query_one("SELECT c.*, u.username, u.name FROM conversations c JOIN users u ON u.id=c.user_id WHERE c.id=?", {id});
        if (c.is_null()) return fail(res, 404, "对话不存在");
        c["messages"] = db.query("SELECT id, role, content, meta, created_at FROM messages WHERE conversation_id=? ORDER BY id", {id});
        reply(res, c);
    });
}

static void register_conversation_routes(httplib::Server& svr) {
    svr.Get("/api/conversations", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        auto u = require_user(db, req, res);
        if (!u) return;
        reply(res, {{"conversations", db.query("SELECT id, title, created_at, updated_at FROM conversations WHERE user_id=? "
                                               "ORDER BY updated_at DESC, id DESC",
                                               {(*u)["id"].get<long long>()})}});
    });

    svr.Get(R"(/api/conversations/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        auto u = require_user(db, req, res);
        if (!u) return;
        long long id = std::stoll(req.matches[1]);
        json c = db.query_one("SELECT id, title, created_at, updated_at FROM conversations WHERE id=? AND user_id=?",
                              {id, (*u)["id"].get<long long>()});
        if (c.is_null()) return fail(res, 404, "对话不存在");
        c["messages"] = db.query("SELECT id, role, content, meta, created_at FROM messages WHERE conversation_id=? ORDER BY id", {id});
        reply(res, c);
    });

    svr.Patch(R"(/api/conversations/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        auto u = require_user(db, req, res);
        if (!u) return;
        std::string title = utf8_prefix(body_json(req).value("title", ""), 60);
        if (title.empty()) return fail(res, 400, "标题不能为空");
        int n = db.exec("UPDATE conversations SET title=? WHERE id=? AND user_id=?",
                        {title, std::stoll(req.matches[1]), (*u)["id"].get<long long>()});
        n ? reply(res, {{"ok", true}}) : fail(res, 404, "对话不存在");
    });

    svr.Delete(R"(/api/conversations/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        auto u = require_user(db, req, res);
        if (!u) return;
        int n = db.exec("DELETE FROM conversations WHERE id=? AND user_id=?", {std::stoll(req.matches[1]), (*u)["id"].get<long long>()});
        n ? reply(res, {{"ok", true}}) : fail(res, 404, "对话不存在");
    });

    // 流式对话：SSE 事件 meta / step / delta / done / error
    svr.Post("/api/chat/stream", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        auto u = require_user(db, req, res);
        if (!u) return;
        json b = body_json(req);
        std::string message = b.value("message", "");
        if (message.empty()) return fail(res, 400, "消息不能为空");
        if (message.size() > 8000) return fail(res, 400, "消息过长");
        long long uid = (*u)["id"].get<long long>();
        long long cid = b.value("conversation_id", 0LL);
        bool created = false;
        if (cid) {
            if (db.query_one("SELECT id FROM conversations WHERE id=? AND user_id=?", {cid, uid}).is_null())
                return fail(res, 404, "对话不存在");
        } else {
            db.exec("INSERT INTO conversations(user_id, title) VALUES(?,?)", {uid, utf8_prefix(message, 30)});
            cid = db.last_insert_id();
            created = true;
        }
        db.exec("INSERT INTO messages(conversation_id, role, content) VALUES(?,?,?)", {cid, std::string("user"), message});
        db.exec("UPDATE conversations SET updated_at=datetime('now','localtime') WHERE id=?", {cid});

        // 组装上下文历史（含本次提问）
        json rows = db.query("SELECT role, content FROM (SELECT id, role, content FROM messages WHERE conversation_id=? "
                             "ORDER BY id DESC LIMIT ?) ORDER BY id",
                             {cid, static_cast<long long>(g_cfg.history_messages)});
        json history = json::array();
        for (const auto& r : rows) history.push_back({{"role", r["role"]}, {"content", r["content"]}});
        std::string username = (*u)["username"];

        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider("text/event-stream; charset=utf-8", [=](size_t, httplib::DataSink& sink) {
            auto emit = [&](const json& ev) {
                if (sink.is_writable()) {
                    std::string s = sse(ev);
                    sink.write(s.data(), s.size());
                }
            };
            emit({{"type", "meta"}, {"conversation_id", cid}, {"created", created}});
            auto t0 = std::chrono::steady_clock::now();
            AgentResult r;
            std::string error;
            try {
                Agent agent(g_cfg.glm, g_cfg.db_path);
                r = agent.run(history, emit);
            } catch (const std::exception& e) {
                error = e.what();
            }
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            Db db2(g_cfg.db_path);
            std::string content = error.empty() ? r.content : (r.content.empty() ? "" : r.content + "\n\n") + "⚠️ " + error;
            json meta = {{"steps", r.steps}, {"products", r.products}, {"elapsed", secs}};
            if (!error.empty()) meta["error"] = error;
            db2.exec("INSERT INTO messages(conversation_id, role, content, meta) VALUES(?,?,?,?)",
                     {cid, std::string("assistant"), content, meta.dump()});
            long long mid = db2.last_insert_id();
            db2.exec("UPDATE conversations SET updated_at=datetime('now','localtime') WHERE id=?", {cid});

            // 对话审计日志（JSON Lines）
            log_line("chat.log", json({{"time", now_str()}, {"user", username}, {"conversation_id", cid}, {"question", message},
                                       {"answer", content}, {"tools", r.steps}, {"elapsed", secs}})
                                     .dump());
            if (!error.empty()) {
                log_info("对话出错 user=" + username + ": " + error);
                emit({{"type", "error"}, {"message", error}});
            }
            emit({{"type", "done"}, {"message_id", mid}, {"products", r.products}, {"steps", r.steps}, {"elapsed", secs}});
            sink.done();
            return true;
        });
    });
}

static void register_product_routes(httplib::Server& svr) {
    svr.Get("/api/categories", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        if (require_user(db, req, res)) reply(res, {{"categories", products::list_categories(db)}});
    });

    svr.Get("/api/products", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        if (!require_user(db, req, res)) return;
        int limit = req.has_param("limit") ? std::stoi(req.get_param_value("limit")) : 50;
        reply(res, {{"products", products::search(db, req.get_param_value("q"), req.get_param_value("category"),
                                                  req.get_param_value("series"), limit)}});
    });

    svr.Get(R"(/api/products/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        if (!require_user(db, req, res)) return;
        json p = products::get(db, req.matches[1]);
        if (p.is_null()) return fail(res, 404, "型号不存在");
        p["replaces"] = competitors::for_tx_model(db, p["model"].get<std::string>());
        reply(res, p);
    });

    // 竞品库（国产替代交叉对照）
    svr.Get("/api/competitors", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        if (!require_user(db, req, res)) return;
        int limit = req.has_param("limit") ? std::stoi(req.get_param_value("limit")) : 100;
        reply(res, {{"competitors", competitors::search(db, req.get_param_value("q"), limit)}});
    });

    // 数据手册下载（需登录）
    svr.Get(R"(/api/datasheets/([^/\\]+\.pdf))", [](const httplib::Request& req, httplib::Response& res) {
        Db db(g_cfg.db_path);
        if (!require_user(db, req, res)) return;
        std::string name = req.matches[1];
        if (name.find("..") != std::string::npos) return fail(res, 400, "非法文件名");
        fs::path p = fs::u8path(g_cfg.datasheet_dir) / fs::u8path(name);
        std::ifstream f(p, std::ios::binary);
        if (!f) return fail(res, 404, "文件不存在");
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        res.set_content(std::move(data), "application/pdf");
    });
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string cfg_path = argc > 1 ? argv[1] : "config.json";
    try {
        load_config(cfg_path);
        fs::create_directories(fs::u8path(g_cfg.log_dir));
        fs::create_directories(fs::u8path(g_cfg.db_path).parent_path());
        init_schema(g_cfg.db_path);
    } catch (const std::exception& e) {
        std::cerr << "启动失败: " << e.what() << std::endl;
        return 1;
    }

    httplib::Server svr;
    svr.set_read_timeout(30);
    svr.set_write_timeout(300);
    svr.set_payload_max_length(1 << 20);

    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        Db db(g_cfg.db_path);
        json s = db.query_one("SELECT (SELECT COUNT(*) FROM products) AS products, (SELECT COUNT(*) FROM users) AS users");
        bool key_ok = !g_cfg.glm.api_key.empty() && g_cfg.glm.api_key.rfind("请填写", 0) != 0;
        reply(res, {{"status", "ok"}, {"model", g_cfg.glm.model}, {"api_key_configured", key_ok}, {"db", s}});
    });

    register_auth_routes(svr);
    register_admin_routes(svr);
    register_conversation_routes(svr);
    register_product_routes(svr);

    svr.set_exception_handler([](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
        std::string msg = "未知错误";
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            msg = e.what();
        } catch (...) {
        }
        log_info("异常 " + req.method + " " + req.path + ": " + msg);
        fail(res, 500, "服务器内部错误: " + msg);
    });
    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        if (req.path != "/api/health") log_line("access.log", now_str() + " " + req.remote_addr + " " + req.method + " " + req.path + " " + std::to_string(res.status));
    });

    bool key_ok = !g_cfg.glm.api_key.empty() && g_cfg.glm.api_key.rfind("请填写", 0) != 0;
    log_info("后端启动 http://" + g_cfg.host + ":" + std::to_string(g_cfg.port) + "  模型=" + g_cfg.glm.model +
             (key_ok ? "" : "  [警告] 未配置 GLM API Key"));
    if (!svr.listen(g_cfg.host, g_cfg.port)) {
        std::cerr << "端口监听失败: " << g_cfg.host << ":" << g_cfg.port << std::endl;
        return 1;
    }
    return 0;
}
