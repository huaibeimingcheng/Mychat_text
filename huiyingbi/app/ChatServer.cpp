#include "ChatServer.h"

#include "../common/Logger.h"
#include "../common/json.h"
#include "../common/protocol.h"
#include "HttpStatic.h"

#include <cstdio>
#include <ctime>
#include <sys/time.h>

namespace {

// 单条消息的限流：5 秒内最多 30 条
constexpr int kRateLimitCount = 30;
constexpr int kRateLimitWindowMs = 5000;

std::string nowTimeStr() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tmv;
    localtime_r(&tv.tv_sec, &tmv);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

// 把连接从聊天室移除并广播离开通知（连接关闭和主动 logout 共用）
} // namespace

ChatServer::ChatServer(EventLoop* mainLoop, Options opt)
    : m_opt(std::move(opt)),
      m_server(mainLoop, m_opt.port, m_opt.ioThreads, m_opt.bindAddr),
      m_users(m_opt.userDbPath) {}

ChatServer::~ChatServer() { stop(); }

bool ChatServer::start() {
    std::string err;
    if (!m_users.load(err)) {
        LOG_ERROR_MSG("加载用户库失败: " << err);
        return false;
    }
    LOG_INFO_MSG("用户库就绪，共 " << m_users.count() << " 个已注册用户");

    m_server.setNewConnectionCallback([this](const std::shared_ptr<Connection>& c) {
        onNewConnection(c);
    });
    m_server.setMessageCallback(
        [this](const std::shared_ptr<Connection>& c, const std::string& t) { onMessage(c, t); });
    m_server.setCloseCallback([this](const std::shared_ptr<Connection>& c) { onClose(c); });

    return m_server.start();
}

void ChatServer::stop() { m_server.stop(); }

// ---------------------------------------------------------------------------
// 连接生命周期
// ---------------------------------------------------------------------------
void ChatServer::onNewConnection(const std::shared_ptr<Connection>& conn) {
    // 同一端口同时提供前端静态页面（非 WebSocket 请求走这条路径）
    conn->setHttpCallback(
        [this](const std::string& req) { return handleStaticHttpRequest(req, m_opt.webRoot); });

    // 欢迎消息必须等 WebSocket 握手完成后再发，否则会插到 101 响应前面
    conn->setConnectedCallback([this](const std::shared_ptr<Connection>& c) {
        JsonValue welcome = JsonValue::makeObject();
        welcome.set("type", protocol::TYPE_SYSTEM);
        welcome.set("text", "欢迎来到聊天室，请先注册或登录");
        welcome.set("needLogin", true);
        welcome.set("history", "聊天室不提供历史消息：进入后只能看到你进入之后的消息");
        c->sendJson(welcome);
    });
}

void ChatServer::onClose(const std::shared_ptr<Connection>& conn) {
    if (!conn->isLoggedIn()) return;

    const std::string name = conn->username();
    m_room.leave(conn);
    conn->setLoggedIn(false);

    if (!name.empty()) {
        broadcastSystem("用户 " + name + " 离开了聊天室");
        broadcastUserList();
        LOG_INFO_MSG("用户断开: " << name << "，在线 " << m_room.onlineCount());
    }
}

// ---------------------------------------------------------------------------
// 消息分发
// ---------------------------------------------------------------------------
void ChatServer::onMessage(const std::shared_ptr<Connection>& conn, const std::string& text) {
    if (!conn->checkRateLimit(kRateLimitCount, kRateLimitWindowMs)) {
        LOG_WARN_MSG("连接 fd=" << conn->fd() << " 触发限流，丢弃消息");
        sendError(conn, "操作过于频繁，请稍后再试");
        return;
    }

    bool ok = false;
    const JsonValue req = JsonValue::parse(text, &ok);
    if (!ok || !req.isObject()) {
        LOG_WARN_MSG("fd=" << conn->fd() << " 收到非法 JSON: " << text.substr(0, 200));
        sendError(conn, "消息格式错误（需要 JSON 对象）");
        return;
    }

    const std::string type = req.getString("type");
    if (type == protocol::TYPE_REGISTER) {
        handleRegister(conn, req);
    } else if (type == protocol::TYPE_LOGIN) {
        handleLogin(conn, req);
    } else if (type == protocol::TYPE_LOGOUT) {
        handleLogout(conn, req);
    } else if (type == protocol::TYPE_CHAT) {
        handleChat(conn, req);
    } else {
        LOG_WARN_MSG("fd=" << conn->fd() << " 未知消息类型: " << type);
        sendError(conn, "未知消息类型: " + type);
    }
}

void ChatServer::handleRegister(const std::shared_ptr<Connection>& conn, const JsonValue& req) {
    const std::string name = req.getString("username");
    const std::string password = req.getString("password");

    if (conn->isLoggedIn()) {
        sendError(conn, "已登录，无需重复注册");
        return;
    }

    std::string err;
    const UserStore::Result r = m_users.registerUser(name, password, err);

    JsonValue resp = JsonValue::makeObject();
    resp.set("type", protocol::TYPE_REGISTER_RESULT);
    resp.set("ok", r == UserStore::Result::Ok);
    resp.set("username", name);
    if (r == UserStore::Result::Ok) {
        resp.set("message", "注册成功，请登录");
        LOG_INFO_MSG("注册成功: " << name << " from " << conn->peer());
    } else {
        resp.set("reason", UserStore::resultText(r));
        LOG_INFO_MSG("注册失败: " << name << " 原因=" << err);
    }
    conn->sendJson(resp);
}

void ChatServer::handleLogin(const std::shared_ptr<Connection>& conn, const JsonValue& req) {
    const std::string name = req.getString("username");
    const std::string password = req.getString("password");

    if (conn->isLoggedIn()) {
        sendError(conn, "你已经登录了");
        return;
    }

    JsonValue resp = JsonValue::makeObject();
    resp.set("type", protocol::TYPE_LOGIN_RESULT);
    resp.set("username", name);

    std::string err;
    if (!UserStore::validUsername(name, err)) {
        resp.set("ok", false);
        resp.set("reason", err);
        conn->sendJson(resp);
        return;
    }

    if (!m_users.verify(name, password)) {
        resp.set("ok", false);
        resp.set("reason", "用户名或密码错误");
        conn->sendJson(resp);
        LOG_WARN_MSG("登录失败: " << name << " from " << conn->peer());
        return;
    }

    // 同一账号不允许重复登录
    if (!m_room.join(conn, name)) {
        resp.set("ok", false);
        resp.set("reason", "该账号已在其它地方登录");
        conn->sendJson(resp);
        LOG_WARN_MSG("重复登录被拒绝: " << name);
        return;
    }

    conn->setUsername(name);
    conn->setLoggedIn(true);

    resp.set("ok", true);
    resp.set("room", "大厅");
    resp.set("message", "登录成功，已进入聊天室");
    resp.set("history", false); // 前端据此提示：不显示历史消息
    JsonValue users = JsonValue::makeArray();
    for (const auto& u : m_room.onlineUsers()) users.push(JsonValue(u));
    resp.set("users", users);
    conn->sendJson(resp);

    LOG_INFO_MSG("登录成功: " << name << " from " << conn->peer() << "，在线 "
                              << m_room.onlineCount());

    broadcastSystem("用户 " + name + " 进入了聊天室");
    broadcastUserList();
}

void ChatServer::handleLogout(const std::shared_ptr<Connection>& conn, const JsonValue& req) {
    (void)req;
    if (!conn->isLoggedIn()) {
        sendError(conn, "尚未登录");
        return;
    }

    const std::string name = conn->username();
    m_room.leave(conn);
    conn->setLoggedIn(false);
    conn->setUsername("");

    JsonValue resp = JsonValue::makeObject();
    resp.set("type", "logout_result");
    resp.set("ok", true);
    resp.set("message", "已退出登录");
    conn->sendJson(resp);

    LOG_INFO_MSG("用户主动登出: " << name);
    broadcastSystem("用户 " + name + " 离开了聊天室");
    broadcastUserList();
}

void ChatServer::handleChat(const std::shared_ptr<Connection>& conn, const JsonValue& req) {
    if (!conn->isLoggedIn()) {
        sendError(conn, "请先登录后再发言");
        return;
    }

    std::string content = req.getString("text");
    if (content.empty()) {
        sendError(conn, "消息内容不能为空");
        return;
    }
    if (content.size() > protocol::MAX_MESSAGE_LEN) {
        sendError(conn, "消息过长（上限 " + std::to_string(protocol::MAX_MESSAGE_LEN) + " 字节）");
        return;
    }

    const std::string name = conn->username();

    JsonValue msg = JsonValue::makeObject();
    msg.set("type", protocol::TYPE_CHAT_MSG);
    msg.set("from", name);
    msg.set("text", content);
    msg.set("time", nowTimeStr());

    // 广播给聊天室内所有人（包含发送者，前端只显示一份）
    m_room.broadcast(msg);

    LOG_DEBUG_MSG("聊天室消息 " << name << ": " << content.substr(0, 80));
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
void ChatServer::sendError(const std::shared_ptr<Connection>& conn, const std::string& reason) {
    conn->sendJson(jsonutil::makeErrorValue(reason));
}

void ChatServer::broadcastSystem(const std::string& text) {
    JsonValue msg = JsonValue::makeObject();
    msg.set("type", protocol::TYPE_SYSTEM);
    msg.set("text", text);
    msg.set("time", nowTimeStr());
    m_room.broadcast(msg);
}

void ChatServer::broadcastUserList() { m_room.broadcast(m_room.userListMessage()); }
