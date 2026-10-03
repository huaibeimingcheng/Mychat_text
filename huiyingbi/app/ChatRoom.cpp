#include "ChatRoom.h"

#include "../common/Logger.h"

#include <algorithm>

bool ChatRoom::join(const std::shared_ptr<Connection>& conn, const std::string& username) {
    if (!conn || username.empty()) return false;

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_users.find(username);
        if (it != m_users.end()) {
            if (auto alive = it->second.lock()) {
                if (alive != conn) return false; // 同名用户已在线
            }
        }
        m_users[username] = conn;
    }

    LOG_INFO_MSG("用户进入聊天室: " << username << "（当前在线 " << onlineCount() << "）");
    return true;
}

void ChatRoom::leave(const std::shared_ptr<Connection>& conn) {
    if (!conn) return;
    std::string removed;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto it = m_users.begin(); it != m_users.end();) {
            const auto alive = it->second.lock();
            if (!alive || alive == conn) {
                if (alive == conn) removed = it->first;
                it = m_users.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!removed.empty()) {
        LOG_INFO_MSG("用户离开聊天室: " << removed << "（当前在线 " << onlineCount() << "）");
    }
}

void ChatRoom::broadcast(const JsonValue& msg, const std::shared_ptr<Connection>& except) {
    const std::string payload = msg.dump();

    std::vector<std::shared_ptr<Connection>> targets;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto it = m_users.begin(); it != m_users.end();) {
            auto alive = it->second.lock();
            if (!alive) {
                it = m_users.erase(it); // 惰性清理已断开的连接
                continue;
            }
            if (alive != except) targets.push_back(alive);
            ++it;
        }
    }

    // 在锁外发送：sendText 内部会投递到各自的 loop 线程
    for (auto& c : targets) {
        c->sendText(payload);
    }
}

std::vector<std::string> ChatRoom::onlineUsers() const {
    std::vector<std::string> names;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& kv : m_users) {
        if (!kv.second.expired()) names.push_back(kv.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

size_t ChatRoom::onlineCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    size_t n = 0;
    for (const auto& kv : m_users) {
        if (!kv.second.expired()) ++n;
    }
    return n;
}

bool ChatRoom::isOnline(const std::string& username) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_users.find(username);
    return it != m_users.end() && !it->second.expired();
}

JsonValue ChatRoom::userListMessage() const {
    JsonValue msg = JsonValue::makeObject();
    msg.set("type", protocol::TYPE_USER_LIST);

    JsonValue arr = JsonValue::makeArray();
    for (const auto& n : onlineUsers()) arr.push(JsonValue(n));
    msg.set("users", arr);
    msg.set("count", static_cast<long long>(arr.size()));
    return msg;
}
