#include "UserStore.h"

#include "../common/Logger.h"
#include "../common/crypto.h"
#include "../common/protocol.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

// 从路径里取出目录部分（"data/users.db" -> "data"）
std::string dirOf(const std::string& path) {
    const size_t p = path.find_last_of('/');
    return (p == std::string::npos) ? std::string() : path.substr(0, p);
}

} // namespace

UserStore::UserStore(std::string dbPath) : m_dbPath(std::move(dbPath)) {}

const char* UserStore::resultText(Result r) {
    switch (r) {
        case Result::Ok:              return "ok";
        case Result::UsernameExists:  return "用户名已被注册";
        case Result::InvalidUsername: return "用户名不合法";
        case Result::InvalidPassword: return "密码不合法";
        case Result::IoError:         return "服务器存储错误";
    }
    return "未知错误";
}

bool UserStore::validUsername(const std::string& name, std::string& err) {
    const size_t n = name.size();
    if (n < 2 || n > protocol::MAX_USERNAME_LEN) {
        err = "用户名长度需在 2~" + std::to_string(protocol::MAX_USERNAME_LEN) + " 字节之间";
        return false;
    }
    for (unsigned char c : name) {
        // 禁止空白与控制字符，其余（含 UTF-8 中文）允许
        if (c < 0x21 || c == 0x7F) {
            err = "用户名不能包含空格或控制字符";
            return false;
        }
        if (c == ':' || c == '\\' || c == '"' || c == '\'') {
            err = "用户名不能包含 : \\ \" ' 等字符";
            return false;
        }
    }
    return true;
}

bool UserStore::validPassword(const std::string& password, std::string& err) {
    const size_t n = password.size();
    if (n < protocol::MIN_PASSWORD_LEN || n > protocol::MAX_PASSWORD_LEN) {
        err = "密码长度需在 " + std::to_string(protocol::MIN_PASSWORD_LEN) + "~" +
              std::to_string(protocol::MAX_PASSWORD_LEN) + " 字节之间";
        return false;
    }
    for (unsigned char c : password) {
        if (c == '\n' || c == '\r' || c == '\t' || c == 0) {
            err = "密码不能包含换行或制表符";
            return false;
        }
    }
    return true;
}

bool UserStore::load(std::string& err) {
    const std::string dir = dirOf(m_dbPath);
    if (!dir.empty()) {
        ::mkdir(dir.c_str(), 0755); // 已存在会返回 EEXIST，忽略
    }

    std::ifstream in(m_dbPath);
    if (!in.is_open()) {
        LOG_INFO_MSG("用户库 " << m_dbPath << " 不存在，将新建");
        return true;
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    std::string line;
    size_t lineno = 0;
    size_t loaded = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string name, salt, hash;
        if (!std::getline(ss, name, '\t') || !std::getline(ss, salt, '\t') || !std::getline(ss, hash)) {
            LOG_WARN_MSG("用户库第 " << lineno << " 行格式错误，已跳过");
            continue;
        }
        m_users[name] = Record{salt, hash};
        ++loaded;
    }
    LOG_INFO_MSG("已从 " << m_dbPath << " 加载 " << loaded << " 个用户");
    (void)err;
    return true;
}

bool UserStore::appendRecord(const std::string& name, const std::string& salt,
                             const std::string& hash) {
    FILE* fp = fopen(m_dbPath.c_str(), "a");
    if (!fp) {
        LOG_ERROR_MSG("无法打开用户库 " << m_dbPath << " 写入");
        return false;
    }
    fprintf(fp, "%s\t%s\t%s\n", name.c_str(), salt.c_str(), hash.c_str());
    fflush(fp);
    fclose(fp);
    return true;
}

UserStore::Result UserStore::registerUser(const std::string& name, const std::string& password,
                                          std::string& err) {
    if (!validUsername(name, err)) return Result::InvalidUsername;
    if (!validPassword(password, err)) return Result::InvalidPassword;

    const std::string salt = crypto::randomHex(16);
    const std::string hash = crypto::hashPassword(salt, password);

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_users.count(name) > 0) {
            err = "用户名已被注册";
            return Result::UsernameExists;
        }
        // 先落盘，成功后才写入内存，避免"内存有、磁盘没有"
        if (!appendRecord(name, salt, hash)) {
            err = "服务器存储错误";
            return Result::IoError;
        }
        m_users[name] = Record{salt, hash};
    }

    LOG_INFO_MSG("新用户注册成功: " << name);
    return Result::Ok;
}

bool UserStore::verify(const std::string& name, const std::string& password) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_users.find(name);
    if (it == m_users.end()) return false;
    return crypto::hashPassword(it->second.salt, password) == it->second.hash;
}

bool UserStore::exists(const std::string& name) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_users.count(name) > 0;
}

size_t UserStore::count() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_users.size();
}
