#pragma once
// 账户认证：PBKDF2-HMAC-SHA256 口令哈希 + 随机 Bearer Token 会话
#include <optional>
#include <string>

#include "db.hpp"

namespace auth {

constexpr int kPbkdf2Iter = 120000;
constexpr long long kSessionTtlSeconds = 12 * 3600;

std::string random_hex(int bytes);
std::string hash_password(const std::string& password, const std::string& salt_hex, int iterations);
bool verify_password(const std::string& password, const json& user_row);
// 返回 (salt, hash)
std::pair<std::string, std::string> make_password(const std::string& password);

std::string create_session(Db& db, long long user_id);
// 校验 token，返回用户行（不含口令字段）
std::optional<json> user_from_token(Db& db, const std::string& token);
void delete_session(Db& db, const std::string& token);

json public_user(const json& row);

}  // namespace auth
