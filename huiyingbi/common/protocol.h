#pragma once

#include <cstddef>

// ============================================================================
// 应用层协议
//
//   传输层：WebSocket（文本帧），握手成功后双方交换 JSON 文本消息
//   消息格式：{"type": "<消息类型>", ...字段}
//
//   客户端 → 服务端：register / login / logout / chat
//   服务端 → 客户端：register_result / login_result / chat / system
//                    user_list / error
//
//   未登录状态下只接受 register / login，其它消息一律回 error。
// ============================================================================
namespace protocol {

// ---- 客户端 → 服务端 ----
constexpr const char* TYPE_REGISTER = "register";
constexpr const char* TYPE_LOGIN    = "login";
constexpr const char* TYPE_LOGOUT   = "logout";
constexpr const char* TYPE_CHAT     = "chat";

// ---- 服务端 → 客户端 ----
constexpr const char* TYPE_REGISTER_RESULT = "register_result";
constexpr const char* TYPE_LOGIN_RESULT    = "login_result";
constexpr const char* TYPE_CHAT_MSG        = "chat";         // 聊天室消息广播
constexpr const char* TYPE_SYSTEM          = "system";       // 系统通知（加入/离开）
constexpr const char* TYPE_USER_LIST       = "user_list";    // 在线用户列表
constexpr const char* TYPE_ERROR           = "error";

// ---- 限制（服务端强制校验，防止恶意客户端撑爆内存）----
constexpr size_t MAX_USERNAME_LEN  = 16;              // 用户名长度上限
constexpr size_t MAX_PASSWORD_LEN  = 64;              // 密码长度上限
constexpr size_t MIN_PASSWORD_LEN  = 6;               // 密码长度下限
constexpr size_t MAX_MESSAGE_LEN   = 2000;            // 单条聊天内容上限（字节）
constexpr size_t MAX_FRAME_SIZE    = 64 * 1024;       // WebSocket 单个消息上限
constexpr size_t MAX_HTTP_HEADER   = 8 * 1024;        // 握手请求头上限
constexpr size_t MAX_WRITE_BUFFER  = 4 * 1024 * 1024; // 单连接写缓冲高水位，超过则断开
constexpr size_t MAX_CONNECTIONS   = 10000;           // 最大并发连接数

// ---- 心跳与超时（秒）----
constexpr int HEARTBEAT_INTERVAL_SEC = 30;   // 服务端主动 ping 间隔
constexpr int IDLE_TIMEOUT_SEC       = 120;  // 超过该时长无任何数据则断开

} // namespace protocol
