#pragma once

#include "../common/Buffer.h"
#include "../common/json.h"
#include "../common/protocol.h"
#include "EventLoop.h"
#include "WebSocket.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

// 一条 TCP 连接：负责 WebSocket 握手、帧收发、心跳与写缓冲背压。
//
// 线程模型：所有 IO 都在构造时传入的 EventLoop 线程里执行；
//           sendText()/close() 是线程安全的，内部会投递到该线程。
class Connection : public std::enable_shared_from_this<Connection> {
public:
    using MessageCallback = std::function<void(const std::shared_ptr<Connection>&, const std::string&)>;
    using CloseCallback = std::function<void(const std::shared_ptr<Connection>&)>;
    // 非 WebSocket 的普通 HTTP 请求（用来在同一端口上提供前端静态页面）
    using HttpCallback = std::function<std::string(const std::string& rawRequest)>;
    // 握手成功、可以开始收业务数据时回调（业务首次发言应在这里发，不能在 start() 时发）
    using ConnectedCallback = std::function<void(const std::shared_ptr<Connection>&)>;

    Connection(int fd, EventLoop* loop, std::string peer);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // 注册到 EventLoop 并开始接收数据（必须在 loop 线程调用）
    void start();

    // 线程安全：发送一条文本消息（WebSocket text 帧）
    void sendText(const std::string& text);
    // 线程安全：发送一个 JSON 对象
    void sendJson(const JsonValue& v) { sendText(v.dump()); }

    // 线程安全：发送完当前写缓冲后关闭
    void closeAfterWrite();

    // ---- 业务字段 ----
    int fd() const { return m_fd; }
    const std::string& peer() const { return m_peer; }
    EventLoop* loop() const { return m_loop; }

    void setUsername(const std::string& name);
    std::string username() const;
    void setLoggedIn(bool v);
    bool isLoggedIn() const;

    // 简单的令牌桶限流：在 windowMs 内最多允许 maxCount 次操作
    bool checkRateLimit(int maxCount, int windowMs);

    bool closed() const { return m_state == State::Closed; }
    size_t writeBufferSize() const { return m_outBuffer.readableBytes(); }

    void setMessageCallback(MessageCallback cb) { m_messageCallback = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { m_closeCallback = std::move(cb); }
    void setHttpCallback(HttpCallback cb) { m_httpCallback = std::move(cb); }
    void setConnectedCallback(ConnectedCallback cb) { m_connectedCallback = std::move(cb); }

private:
    enum class State { Handshaking, Open, Closed };

    void handleEvent(uint32_t events);
    void handleRead();
    void handleWrite();

    void doHandshake();
    void processFrames();
    void handleFrame(const websocket::Frame& frame);
    void handleDataMessage(const std::string& text);

    void sendInLoop(std::string data);
    void closeInLoop();
    void enableWriting();
    void disableWriting();
    void scheduleHeartbeat();

    int m_fd;
    EventLoop* m_loop;
    std::string m_peer;

    Buffer m_inBuffer;
    Buffer m_outBuffer;

    State m_state = State::Handshaking;
    bool m_writing = false;
    bool m_closeAfterWrite = false;
    bool m_closeNotified = false;

    // WebSocket 分片重组
    bool m_fragmented = false;
    std::string m_fragPayload;

    int64_t m_lastActiveMs = 0;

    // 限流窗口
    int64_t m_rateWindowStart = 0;
    int m_rateCount = 0;

    // 业务字段（可能被其它线程读，需要加锁）
    mutable std::mutex m_userMutex;
    std::string m_username;
    bool m_loggedIn = false;

    MessageCallback m_messageCallback;
    CloseCallback m_closeCallback;
    ConnectedCallback m_connectedCallback;
    HttpCallback m_httpCallback;
};
