#pragma once
#include <sys/epoll.h>
#include <unordered_map>
#include <memory>
#include <vector>
#include "Connection.h"

class Server {
public:
    Server(int port);
    ~Server();
    void start();

    // 由 Connection 调用：发送缓存未清空时注册 EPOLLOUT，清空后摘除
    void enableWrite(int fd);
    void disableWrite(int fd);
    // 由 Connection 调用：把消息广播给所有连接（exclude_fd = -1 表示不排除任何人）
    void broadcast(int exclude_fd, uint16_t msgId, const std::string& body);

private:
    int m_port;
    int m_listenFd;
    int m_epollFd;
    bool m_isRunning;

    std::unordered_map<int, std::shared_ptr<Connection>> m_connections;

    void initListenSocket();
    void setNonBlocking(int fd);
    void addToEpoll(int fd, uint32_t events);
    void modifyEpoll(int fd, uint32_t events);

    void handleNewConnection();
    void handleCloseConnection(int fd);
    void handleEvent(int fd, uint32_t events);
};