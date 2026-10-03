#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

// 用户账号存储：
//   - 内存里是 unordered_map<用户名, {salt, hash}>
//   - 落盘为 data/users.db，每行 "用户名\tsalt\tsha256(salt:密码)"
//   - 密码绝不明文保存；所有公开接口线程安全
class UserStore {
public:
    enum class Result {
        Ok = 0,
        UsernameExists,
        InvalidUsername,
        InvalidPassword,
        IoError
    };

    explicit UserStore(std::string dbPath);

    bool load(std::string& err);

    Result registerUser(const std::string& name, const std::string& password, std::string& err);
    bool verify(const std::string& name, const std::string& password) const;
    bool exists(const std::string& name) const;
    size_t count() const;

    static bool validUsername(const std::string& name, std::string& err);
    static bool validPassword(const std::string& password, std::string& err);
    static const char* resultText(Result r);

private:
    bool appendRecord(const std::string& name, const std::string& salt, const std::string& hash);

    struct Record {
        std::string salt;
        std::string hash;
    };

    std::string m_dbPath;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Record> m_users;
};
