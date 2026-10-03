#pragma once

#include "Connection.h"
#include "EventLoop.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// 多 Reactor 服务器：
//   - 主线程（mainLoop）只负责 accept 新连接
//   - accept 到的 fd 按 round-robin 分发给 N 个 IO 线程，
//     每个 IO 线程有独立的 EventLoop(epoll)，连接的读写都在其所属线程完成
class TcpServer {
public:
    using NewConnectionCallback = std::function<void(const std::shared_ptr<Connection>&)>;
    using MessageCallback = std::function<void(const std::shared_ptr<Connection>&, const std::string&)>;
    using CloseCallback = std::function<void(const std::shared_ptr<Connection>&)>;

    // mainLoop 由调用方提供（通常是 main 线程的 EventLoop）；ioThreadCount 为 IO 线程数
    // bindAddr：监听地址，默认 "0.0.0.0"（监听本机所有网卡，外部机器可连）；
    //           只允许本机访问时传 "127.0.0.1"
    TcpServer(EventLoop* mainLoop, int port, int ioThreadCount, std::string bindAddr = "0.0.0.0");
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool start();  // 创建监听 socket 与 IO 线程，失败返回 false
    void stop();

    void setNewConnectionCallback(NewConnectionCallback cb) { m_newConnCb = std::move(cb); }
    void setMessageCallback(MessageCallback cb) { m_messageCb = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { m_closeCb = std::move(cb); }

    EventLoop* mainLoop() const { return m_mainLoop; }
    size_t connectionCount() const;
    int port() const { return m_port; }
    const std::string& bindAddr() const { return m_bindAddr; }

    // 遍历所有在线连接（会先拷贝一份 shared_ptr 列表，回调里可以安全地在其它线程 send）
    void forEachConnection(const std::function<void(const std::shared_ptr<Connection>&)>& fn) const;

    // 线程安全：从服务器登记表中移除连接（连接关闭时由上层调用）
    void removeConnection(const std::shared_ptr<Connection>& conn);

private:
    int createListenSocket();
    void handleAccept();
    void dispatchToLoop(int fd, const std::string& peer);
    EventLoop* pickLoop();

    EventLoop* m_mainLoop;
    int m_port;
    int m_ioThreadCount;
    std::string m_bindAddr;
    int m_listenFd = -1;

    std::vector<std::unique_ptr<EventLoopThread>> m_ioThreads;
    std::vector<EventLoop*> m_loops;
    std::atomic<size_t> m_nextLoop{0};

    mutable std::mutex m_connMutex;
    std::set<std::shared_ptr<Connection>> m_connections;

    NewConnectionCallback m_newConnCb;
    MessageCallback m_messageCb;
    CloseCallback m_closeCb;

    std::atomic<bool> m_started{false};
    std::atomic<size_t> m_acceptCount{0};
};
