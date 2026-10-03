#include "Server.h"
#include "protocol.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>

#define MAX_EVENTS 1024

Server::Server(int port)
    : m_port(port), m_listenFd(-1), m_epollFd(-1), m_isRunning(false) {}

Server::~Server() {
    if (m_listenFd != -1) close(m_listenFd);
    if (m_epollFd != -1) close(m_epollFd);
}

void Server::initListenSocket() {
    m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenFd == -1) { std::cerr << "socket 创建失败!" << std::endl; exit(1); }

    int opt = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_port);

    if (bind(m_listenFd, (sockaddr*)&addr, sizeof(addr)) == -1) {
        std::cerr << "bind 失败，端口可能被占用!" << std::endl; exit(1);
    }
    if (listen(m_listenFd, SOMAXCONN) == -1) { std::cerr << "listen 失败!" << std::endl; exit(1); }

    setNonBlocking(m_listenFd);
    std::cout << "服务器启动，监听端口 " << m_port << " ..." << std::endl;
}

void Server::setNonBlocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
}

void Server::addToEpoll(int fd, uint32_t events) {
    epoll_event ev; ev.data.fd = fd; ev.events = events;
    epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &ev);
}

void Server::modifyEpoll(int fd, uint32_t events) {
    epoll_event ev; ev.data.fd = fd; ev.events = events;
    epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev);
}

// 写缓存还有积压：加入 EPOLLOUT，等内核可写时继续发
void Server::enableWrite(int fd) {
    if (m_connections.find(fd) == m_connections.end()) return;
    modifyEpoll(fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLOUT);
}

// 写缓存已清空：摘掉 EPOLLOUT，避免空转唤醒
void Server::disableWrite(int fd) {
    if (m_connections.find(fd) == m_connections.end()) return;
    modifyEpoll(fd, EPOLLIN | EPOLLET | EPOLLRDHUP);
}

void Server::start() {
    initListenSocket();
    m_epollFd = epoll_create1(0);
    addToEpoll(m_listenFd, EPOLLIN | EPOLLET);
    m_isRunning = true;
    std::vector<epoll_event> events(MAX_EVENTS);

    while (m_isRunning) {
        int num = epoll_wait(m_epollFd, events.data(), MAX_EVENTS, -1);
        if (num == -1) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < num; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;
            if (fd == m_listenFd) { handleNewConnection(); }
            else { handleEvent(fd, ev); }
        }
    }
}

void Server::handleNewConnection() {
    while (true) { // ⚠️ EPOLLET 模式下，accept 必须循环到 EAGAIN
        sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(m_listenFd, (sockaddr*)&client_addr, &len);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            std::cerr << "accept 错误" << std::endl; break;
        }
        setNonBlocking(client_fd);
        // 注意：必须把 this 传进去，Connection 内部要靠它广播 / 挂 EPOLLOUT
        std::shared_ptr<Connection> conn = std::make_shared<Connection>(client_fd, this);
        m_connections[client_fd] = conn;
        addToEpoll(client_fd, EPOLLIN | EPOLLET | EPOLLRDHUP);
        std::cout << "新客户端连接: fd=" << client_fd << " IP=" << inet_ntoa(client_addr.sin_addr) << std::endl;

        // 新用户加入：广播给所有人（-1 代表不排除任何人）
        std::string notice = "用户 " + std::to_string(client_fd) + " 加入了聊天室";
        broadcast(-1, protocol::MSG_SYSTEM, notice);
    }
}

void Server::handleEvent(int fd, uint32_t events) {
    auto it = m_connections.find(fd);
    if (it == m_connections.end()) return;
    std::shared_ptr<Connection> conn = it->second;

    // 可读（含对端半关闭 EPOLLRDHUP）：交给 handleRead，
    // readFd 返回 0 时会内部 closeConnection，由下面统一收尾
    if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
        conn->handleRead();
    }
    if (conn->isClosed()) { handleCloseConnection(fd); return; }

    // 可写：把 sendMessage 里没发完的剩余数据续发出去
    if (events & EPOLLOUT) {
        conn->handleWrite();
    }
    if (conn->isClosed()) { handleCloseConnection(fd); return; }
}

void Server::handleCloseConnection(int fd) {
    auto it = m_connections.find(fd);
    if (it == m_connections.end()) return;

    std::shared_ptr<Connection> conn = it->second; // 先保住引用，防止析构后还用
    m_connections.erase(it);                       // 先从连接表摘除，广播时不会再发给自己

    epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr);
    std::cout << "客户端断开: fd=" << fd << std::endl;

    // 用户离开：广播给剩下的人
    std::string notice = "用户 " + std::to_string(fd) + " 离开了聊天室";
    broadcast(-1, protocol::MSG_SYSTEM, notice);

    // conn 离开作用域，引用计数归零 → ~Connection() 里 close(fd)
}

void Server::broadcast(int exclude_fd, uint16_t msgId, const std::string& body) {
    for (auto& [fd, conn] : m_connections) {
        if (fd == exclude_fd) continue;   // exclude_fd 传 -1 时表示"所有人都发"
        if (conn->isClosed()) continue;   // 已标记关闭的连接跳过
        conn->sendMessage(msgId, body);
    }
}