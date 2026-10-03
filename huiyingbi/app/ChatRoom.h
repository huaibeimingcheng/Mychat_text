#pragma once

#include "../common/json.h"
#include "../net/Connection.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// 固定的单个聊天室（全局唯一，没有房间概念）。
//
// 重要设计：**不保存任何历史消息**。
//   - 服务器只做"实时转发"：消息发出去就没了，不落盘、不缓存
//   - 新进入的用户拿不到进入之前的任何聊天内容，只能看到自己进来之后的消息
class ChatRoom {
public:
    // 加入聊天室；同名用户已在线时返回 false
    bool join(const std::shared_ptr<Connection>& conn, const std::string& username);

    // 离开聊天室（连接断开或主动 logout 都会调用）
    void leave(const std::shared_ptr<Connection>& conn);

    // 向聊天室内所有人广播（except 为空表示包括发送者自己）
    void broadcast(const JsonValue& msg, const std::shared_ptr<Connection>& except = nullptr);

    std::vector<std::string> onlineUsers() const;
    size_t onlineCount() const;
    bool isOnline(const std::string& username) const;

    // 构造 user_list 消息
    JsonValue userListMessage() const;

private:
    mutable std::mutex m_mutex;
    // 用户名 -> 连接（弱引用，连接析构后自动失效）
    std::unordered_map<std::string, std::weak_ptr<Connection>> m_users;
};
