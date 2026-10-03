#pragma once
#include <cstdint>

// 自定义应用层协议：| magic(2B) | msgId(2B) | bodyLen(4B) | body(bodyLen B) |
// 所有多字节字段统一使用网络字节序（大端）
namespace protocol {

    constexpr uint16_t MAGIC        = 0xEB90;
    constexpr size_t   HEADER_LEN   = 8;                 // 2 + 2 + 4
    constexpr uint32_t MAX_BODY_LEN = 1024 * 1024;       // 包体上限 1MB

    // 消息号
    constexpr uint16_t MSG_CHAT   = 1;  // 聊天消息
    constexpr uint16_t MSG_SYSTEM = 2;  // 系统通知（服务器 → 客户端）

} // namespace protocol
