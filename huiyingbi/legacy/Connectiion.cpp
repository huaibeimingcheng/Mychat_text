#include "Connection.h"
#include "Server.h"   // m_server->broadcast / enableWrite 需要完整类型
#include <errno.h>

// 初始化顺序必须与 Connection.h 中的成员声明顺序一致，否则 -Wreorder 告警
Connection::Connection(int fd,Server* server)
    : m_fd(fd), m_isClosed(false), m_state(ParseState::HEADER),
      m_msgId(0), m_bodyLen(0), m_server(server) {}

Connection::~Connection() { closeConnection(); }

void Connection::closeConnection() {
    if (!m_isClosed && m_fd >= 0) {
        close(m_fd);
        m_isClosed = true;
        std::cout << "fd " << m_fd << " 已关闭" << std::endl;
    }
}

void Connection::handleRead() {
    // ⚠️ EPOLLET 模式下必须循环读到 EAGAIN：
    // 只 read 一次的话，内核缓冲区里剩下没读走的数据不会再触发新事件（除非对端又发数据），
    // 表现为"偶发丢消息 / 卡住不解析"。
    while (true) {
        ssize_t n = m_readBuffer.readFd(m_fd);
        if (n > 0) {
            parseMessage();
            if (m_isClosed) return; // 解析时发现非法魔数/超大包，已主动断开
            continue;               // 继续读，直到 EAGAIN
        }
        if (n == 0) {
            std::cout << "客户端断开连接: fd=" << m_fd << std::endl;
            closeConnection();
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return; // 本轮数据读干净了
        if (errno == EINTR) continue;                        // 被信号打断，重试
        std::cerr << "读错误 fd=" << m_fd << " errno=" << errno << std::endl;
        closeConnection();
        return;
    }
}

void Connection::parseMessage() {
    while (true) {
        if (m_state == ParseState::HEADER) {
            if (m_readBuffer.readableBytes() < protocol::HEADER_LEN) return;

            uint16_t magic, msgId;
            uint32_t bodyLen;
            memcpy(&magic, m_readBuffer.peek(), 2);
            memcpy(&msgId, m_readBuffer.peek() + 2, 2);
            memcpy(&bodyLen, m_readBuffer.peek() + 4, 4);

            magic = ntohs(magic);
            m_msgId = ntohs(msgId);
            m_bodyLen = ntohl(bodyLen);

            if (magic != protocol::MAGIC) {
                std::cerr << "非法魔数，关闭连接" << std::endl;
                closeConnection(); return;
            }
            if (m_bodyLen > protocol::MAX_BODY_LEN) { // 防御：包体不能超过 1MB
                std::cerr << "包体过大，关闭连接" << std::endl;
                closeConnection(); return;
            }
            m_readBuffer.retrieve(protocol::HEADER_LEN);
            m_state = ParseState::BODY;
        }

        if (m_state == ParseState::BODY) {
            if (m_readBuffer.readableBytes() < m_bodyLen) return;
            m_body.assign(m_readBuffer.peek(), m_bodyLen);
            m_readBuffer.retrieve(m_bodyLen);
            m_state = ParseState::COMPLETE;
        }

        if (m_state == ParseState::COMPLETE) {
            if (m_msgId == protocol::MSG_CHAT) {
                // 聊天消息：交给 Server 广播
                m_server->broadcast(m_fd, m_msgId, m_body);
            } else if (m_msgId == protocol::MSG_SYSTEM) {
                // 系统消息：通常由服务器自己发，客户端一般不发这个
                std::cerr << "忽略客户端发来的系统消息 fd=" << m_fd << std::endl;
            } else {
                std::cerr << "未知 msgId=" << m_msgId << " fd=" << m_fd << std::endl;
            }
            m_state = ParseState::HEADER;
            m_body.clear();
        }
    }
}

void Connection::sendMessage(unsigned short msgId, const std::string& jsonBody) {
    if (m_isClosed) return;

    uint16_t magic = htons(protocol::MAGIC);
    uint16_t mid   = htons(msgId);
    uint32_t blen  = htonl(static_cast<uint32_t>(jsonBody.size()));

    // 统一走小端/大端无关的拷贝：先写头，再写体（都进入写缓存，粘包由接收方按长度拆）
    m_writeBuffer.append(reinterpret_cast<const char*>(&magic), sizeof(magic));
    m_writeBuffer.append(reinterpret_cast<const char*>(&mid), sizeof(mid));
    m_writeBuffer.append(reinterpret_cast<const char*>(&blen), sizeof(blen));
    m_writeBuffer.append(jsonBody.data(), jsonBody.size());

    handleWrite(); // 能直接发完就发完，发不完 handleWrite 内部会挂 EPOLLOUT
}

void Connection::handleWrite() {
    if (m_isClosed) return;

    while (m_writeBuffer.readableBytes() > 0) {
        ssize_t n = send(m_fd, m_writeBuffer.peek(), m_writeBuffer.readableBytes(), MSG_NOSIGNAL);
        if (n > 0) {
            m_writeBuffer.retrieve(static_cast<size_t>(n));
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // ⚠️ 内核发送缓冲区已满，数据没发完。
                // ET 模式下不会再有新事件靠"数据到达"触发，必须主动挂上 EPOLLOUT，
                // 否则剩余数据会永久滞留。
                if (m_server) m_server->enableWrite(m_fd);
                return;
            }
            if (errno == EINTR) continue; // 被信号打断，重试
            std::cerr << "写错误 fd=" << m_fd << " errno=" << errno << std::endl;
            closeConnection(); return;
        } else {
            // n == 0：发送非空数据时理论上不会出现，视为对端异常
            std::cerr << "send 返回 0，fd=" << m_fd << std::endl;
            closeConnection(); return;
        }
    }

    // 全部发完：摘掉 EPOLLOUT，否则水平/边沿都会造成无意义的唤醒
    if (m_server) m_server->disableWrite(m_fd);
}