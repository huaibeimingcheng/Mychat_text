#pragma once
#include <vector>
#include <string>
#include <sys/uio.h> // readv 需要用到
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <errno.h>

class Buffer {
public:
    Buffer(int initSize = 1024);
    ~Buffer() = default;

    // 获取当前可读数据的字节数
    size_t readableBytes() const;
    // 获取当前可写空间的字节数
    size_t writableBytes() const;
    // 返回可读数据的起始指针（给 readv/send 使用）
    const char* peek() const;
    // 消费掉 len 字节的数据（移动读指针，逻辑删除）
    void retrieve(size_t len);
    // 追加数据到缓冲区（测试用）
    void append(const char* data, size_t len);
    // 从 socket 读取数据（核心，使用 readv 处理半包扩容）
    ssize_t readFd(int fd);

private:
    std::vector<char> m_data;    // 实际存储字节的容器
    size_t m_read_index;         // 读指针（已读数据的位置）
    size_t m_write_index;        // 写指针（已写数据的位置）
};