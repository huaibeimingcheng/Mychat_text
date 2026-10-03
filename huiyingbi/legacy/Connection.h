#pragma once
#include "Buffer.h"
#include "protocol.h"
#include <string>
#include <memory>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

class Server; // 前向声明：Server.h 里会包含本头文件，不能反向包含

enum class ParseState { HEADER, BODY, COMPLETE };

class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(int fd,Server* server);
    ~Connection();

    int getFd() const { return m_fd; }
    bool isClosed() const { return m_isClosed; }
    void handleRead();
    void handleWrite();
    void sendMessage(unsigned short msgId, const std::string& jsonBody);
    void closeConnection();


private:
    void parseMessage(); // 核心拆包状态机

    int m_fd;
    bool m_isClosed;
    Buffer m_readBuffer;
    Buffer m_writeBuffer;

    ParseState m_state;
    unsigned short m_msgId;
    unsigned int m_bodyLen;
    std::string m_body;
    Server* m_server;
};