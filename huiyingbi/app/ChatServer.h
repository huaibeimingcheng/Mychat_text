#pragma once

#include "../net/TcpServer.h"
#include "ChatRoom.h"
#include "UserStore.h"

#include <memory>
#include <string>

// 业务层：把 TcpServer(WebSocket) + UserStore(注册登录) + ChatRoom(单聊天室) 组装起来。
//
// 连接级状态机：
//   握手成功 → 未登录（只能 register / login）
//            → login 成功 → 已登录（可 chat / logout）→ 进入固定聊天室
class ChatServer {
public:
    struct Options {
        int port = 8888;
        std::string bindAddr = "0.0.0.0"; // 监听地址：0.0.0.0=所有网卡，127.0.0.1=仅本机
        int ioThreads = 4;
        std::string userDbPath = "data/users.db";
        std::string webRoot = "web";
    };

    ChatServer(EventLoop* mainLoop, Options opt);
    ~ChatServer();

    bool start();
    void stop();

    size_t onlineCount() const { return m_room.onlineCount(); }
    size_t connectionCount() const { return m_server.connectionCount(); }
    size_t userCount() const { return m_users.count(); }
    int port() const { return m_opt.port; }
    const std::string& bindAddr() const { return m_server.bindAddr(); }

private:
    void onNewConnection(const std::shared_ptr<Connection>& conn);
    void onMessage(const std::shared_ptr<Connection>& conn, const std::string& text);
    void onClose(const std::shared_ptr<Connection>& conn);

    void handleRegister(const std::shared_ptr<Connection>& conn, const JsonValue& req);
    void handleLogin(const std::shared_ptr<Connection>& conn, const JsonValue& req);
    void handleLogout(const std::shared_ptr<Connection>& conn, const JsonValue& req);
    void handleChat(const std::shared_ptr<Connection>& conn, const JsonValue& req);

    void sendError(const std::shared_ptr<Connection>& conn, const std::string& reason);
    void broadcastSystem(const std::string& text);
    void broadcastUserList();

    Options m_opt;
    TcpServer m_server;
    UserStore m_users;
    ChatRoom m_room;
};
