#pragma once

#include <algorithm>
#include <cstring>
#include <string>
#include <sys/uio.h> // readv
#include <unistd.h>
#include <vector>

// 应用层输入/输出缓冲区。
// 布局： [ 已读空闲区 | 可读数据 | 可写空闲区 ]
//         0        m_read_index  m_write_index   m_data.size()
class Buffer {
public:
    explicit Buffer(size_t initSize = 1024) : m_data(initSize), m_read_index(0), m_write_index(0) {}

    size_t readableBytes() const { return m_write_index - m_read_index; }
    size_t writableBytes() const { return m_data.size() - m_write_index; }
    size_t prependableBytes() const { return m_read_index; }

    const char* peek() const { return m_data.data() + m_read_index; }

    // 在缓冲区头部预留的空间里向前扩展（WebSocket 组帧时用得少，保留能力）
    void prepend(const char* data, size_t len) {
        if (len > prependableBytes()) return;
        m_read_index -= len;
        std::copy(data, data + len, m_data.begin() + m_read_index);
    }

    void retrieve(size_t len) {
        if (len >= readableBytes()) {
            retrieveAll();
            return;
        }
        m_read_index += len;
    }

    void retrieveAll() {
        m_read_index = 0;
        m_write_index = 0;
    }

    std::string retrieveAllAsString() {
        std::string s(peek(), readableBytes());
        retrieveAll();
        return s;
    }

    // 把 data 追加到缓冲区；空间不足时先搬移未读数据、再按需扩容。
    void append(const char* data, size_t len) {
        if (len == 0) return;
        if (writableBytes() < len) {
            makeSpace(len);
        }
        std::copy(data, data + len, m_data.begin() + m_write_index);
        m_write_index += len;
    }

    void append(const std::string& s) { append(s.data(), s.size()); }

    // 从 socket 读取数据，利用 readv 两段式读，避免频繁扩容。
    // 返回：>0 读到的字节数，0 对端关闭，-1 出错（errno 保存在 savedErrno）
    ssize_t readFd(int fd, int* savedErrno = nullptr);

    // 确保有 len 字节可写空间（WebSocket 组帧时先算长度再写）
    void ensureWritableBytes(size_t len) {
        if (writableBytes() < len) makeSpace(len);
    }

    char* beginWrite() { return m_data.data() + m_write_index; }
    const char* beginWrite() const { return m_data.data() + m_write_index; }
    void hasWritten(size_t len) { m_write_index += len; }

private:
    void makeSpace(size_t len) {
        // ⚠️ 关键：搬移"未读数据到最前面"能腾出的空间就是 writableBytes()（= size - readable），
        // 它本来就 < len，光搬移根本不够。判定必须是"搬移后的总容量是否 >= readable + len"。
        // （曾经写成 `readable + writable >= len`：该分支恒为真，搬移后照样越界写，踩坏堆，
        //   症状是服务器 double free or corruption / munmap_chunk(): invalid pointer 后 SIGABRT）
        size_t readable = readableBytes();
        if (m_data.size() < readable + len) {
            // 扩容：小缓冲区按 2 倍增长，减少反复拷贝；大缓冲区按需精确增长
            size_t newSize = m_data.size() * 2;
            if (newSize < readable + len) newSize = readable + len;
            m_data.resize(newSize);
        }
        std::copy(m_data.begin() + m_read_index, m_data.begin() + m_write_index, m_data.begin());
        m_read_index = 0;
        m_write_index = readable;
    }

    std::vector<char> m_data;
    size_t m_read_index;
    size_t m_write_index;
};
