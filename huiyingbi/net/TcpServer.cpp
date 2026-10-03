#include "TcpServer.h"

#include "../common/Logger.h"
#include "../common/protocol.h"

#include <arpa/inet.h>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

TcpServer::TcpServer(EventLoop* mainLoop, int port, int ioThreadCount, std::string bindAddr)
    : m_mainLoop(mainLoop),
      m_port(port),
      m_ioThreadCount(ioThreadCount < 1 ? 1 : ioThreadCount),
      m_bindAddr(bindAddr.empty() ? std::string("0.0.0.0") : std::move(bindAddr)) {}

TcpServer::~TcpServer() { stop(); }

int TcpServer::createListenSocket() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOG_ERROR_MSG("socket 创建失败: " << strerror(errno));
        return -1;
    }

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    // 监听地址：0.0.0.0 = INADDR_ANY，绑定本机所有网卡，局域网/外网都能连进来；
    // 想只允许本机访问就传 127.0.0.1。
    if (::inet_pton(AF_INET, m_bindAddr.c_str(), &addr.sin_addr) != 1) {
        LOG_ERROR_MSG("监听地址不合法: " << m_bindAddr
                                         << "（需要 IPv4 点分十进制，如 0.0.0.0 / 192.168.1.10）");
        ::close(fd);
        return -1;
    }
    addr.sin_port = htons(static_cast<uint16_t>(m_port));

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR_MSG("bind " << m_bindAddr << ":" << m_port << " 失败: " << strerror(errno));
        ::close(fd);
        return -1;
    }
    if (::listen(fd, SOMAXCONN) < 0) {
        LOG_ERROR_MSG("listen 失败: " << strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

bool TcpServer::start() {
    if (m_started.exchange(true)) return true;

    m_listenFd = createListenSocket();
    if (m_listenFd < 0) {
        m_started.store(false);
        return false;
    }

    // 启动 IO 线程池
    for (int i = 0; i < m_ioThreadCount; ++i) {
        auto t = std::make_unique<EventLoopThread>("io-" + std::to_string(i));
        EventLoop* loop = t->start();
        m_loops.push_back(loop);
        m_ioThreads.push_back(std::move(t));
    }

    m_mainLoop->runInLoop([this]() {
        m_mainLoop->addFd(m_listenFd, EPOLLIN | EPOLLET, [this](uint32_t) { handleAccept(); });
    });

    LOG_INFO_MSG("TcpServer 启动，监听 " << m_bindAddr << ":" << m_port << "，IO 线程数 "
                                        << m_loops.size());
    return true;
}

void TcpServer::stop() {
    if (!m_started.exchange(false)) return;

    if (m_listenFd >= 0) {
        m_mainLoop->runInLoop([this]() {
            m_mainLoop->delFd(m_listenFd);
            if (m_listenFd >= 0) {
                ::close(m_listenFd);
                m_listenFd = -1;
            }
        });
    }

    for (auto& t : m_ioThreads) {
        if (t) t.reset(); // EventLoopThread 析构里会 quit + join
    }
    m_ioThreads.clear();
    m_loops.clear();

    std::lock_guard<std::mutex> lk(m_connMutex);
    m_connections.clear();
}

EventLoop* TcpServer::pickLoop() {
    if (m_loops.empty()) return m_mainLoop;
    const size_t idx = m_nextLoop.fetch_add(1) % m_loops.size();
    return m_loops[idx];
}

void TcpServer::handleAccept() {
    // EPOLLET：accept 必须循环到 EAGAIN
    while (true) {
        sockaddr_in peer;
        socklen_t len = sizeof(peer);
        const int fd = ::accept4(m_listenFd, reinterpret_cast<sockaddr*>(&peer), &len,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EMFILE || errno == ENFILE) {
                LOG_ERROR_MSG("文件描述符耗尽，无法 accept");
                break;
            }
            LOG_WARN_MSG("accept 失败: " << strerror(errno));
            break;
        }

        if (connectionCount() >= protocol::MAX_CONNECTIONS) {
            LOG_WARN_MSG("连接数已达上限 " << protocol::MAX_CONNECTIONS << "，拒绝新连接");
            ::close(fd);
            continue;
        }

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        const std::string peerStr = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));

        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        dispatchToLoop(fd, peerStr);
    }
}

void TcpServer::dispatchToLoop(int fd, const std::string& peer) {
    EventLoop* loop = pickLoop();
    // TcpServer 的生命周期由 main 持有（比所有连接长），这里捕获 this 是安全的
    loop->runInLoop([this, fd, peer, loop]() {
        auto conn = std::make_shared<Connection>(fd, loop, peer);
        conn->setMessageCallback(m_messageCb);

        conn->setCloseCallback([this](const std::shared_ptr<Connection>& c) {
            if (m_closeCb) m_closeCb(c);
            removeConnection(c);
        });

        {
            std::lock_guard<std::mutex> lk(m_connMutex);
            m_connections.insert(conn);
        }

        conn->start();

        if (m_newConnCb) m_newConnCb(conn);
    });
}

void TcpServer::removeConnection(const std::shared_ptr<Connection>& conn) {
    if (!conn) return;

    {
        std::lock_guard<std::mutex> lk(m_connMutex);
        m_connections.erase(conn);
    }

    // 若当前不在连接所属的 loop 线程（例如被其它线程强制关闭），
    // 就把"释放最后一个引用"的动作投递到它的 loop 线程，保证对象在线程内析构。
    EventLoop* loop = conn->loop();
    if (loop && !loop->isInLoopThread()) {
        std::weak_ptr<Connection> weak = conn;
        loop->runInLoop([weak]() {
            if (auto self = weak.lock()) {
                // 任务结束后 self 析构，真正释放
            }
        });
    }
}

size_t TcpServer::connectionCount() const {
    std::lock_guard<std::mutex> lk(m_connMutex);
    return m_connections.size();
}

void TcpServer::forEachConnection(
    const std::function<void(const std::shared_ptr<Connection>&)>& fn) const {
    if (!fn) return;
    std::vector<std::shared_ptr<Connection>> snapshot;
    {
        std::lock_guard<std::mutex> lk(m_connMutex);
        snapshot.assign(m_connections.begin(), m_connections.end());
    }
    for (const auto& c : snapshot) {
        if (c && !c->closed()) fn(c);
    }
}
