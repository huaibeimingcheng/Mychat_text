#pragma once

#include <string>

// 本文件集中放置协议/安全相关的哈希与编码工具，均为轻量自实现，无第三方依赖。

namespace crypto {

// ---- SHA1（WebSocket 握手用）----
// 返回 20 字节原始摘要
std::string sha1Raw(const std::string& data);

// ---- SHA256（密码存储用）----
// 返回 32 字节原始摘要
std::string sha256Raw(const std::string& data);
// 返回 64 个十六进制字符
std::string sha256Hex(const std::string& data);

// ---- Base64 ----
std::string base64Encode(const std::string& raw);

// ---- 随机数 ----
// 返回 2*bytes 个十六进制字符（用于密码盐、会话 token）
std::string randomHex(size_t bytes);

// 密码加盐哈希：sha256(salt + ":" + password) 的十六进制形式
std::string hashPassword(const std::string& salt, const std::string& password);

} // namespace crypto
