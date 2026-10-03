#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
回音壁 (Echo Server) 协议测试脚本
=================================================================

被测服务：C++ 版 MyChatServer（epoll + 非阻塞 + 自定义协议）
默认监听：127.0.0.1:8888

协议格式（大端序 Big-Endian）::

    +-------------+-------------+---------------+--------------------+
    | magic (2B)  | msgId (2B)  | bodyLen (4B)  | body (bodyLen 字节) |
    +-------------+-------------+---------------+--------------------+
    |   0xEB90    |   消息 ID    |   包体长度     |   JSON / 字节流     |
    +-------------+-------------+---------------+--------------------+

服务端行为：把收到的 msgId 与包体**原样发回**（回音壁）。

用法::

    python3 test_echo_server.py                    # 测试 127.0.0.1:8888
    python3 test_echo_server.py -p 9999            # 指定端口
    python3 test_echo_server.py -H 192.168.1.7 -p 8888   # 指定主机

退出码：0 表示全部通过，1 表示存在失败用例。

注意：用例 13（256KB 大包体）目前会触发服务端 `Buffer::append` 的堆越界写
（`readable + writable >= len` 判定成立、但搬移后剩余空间其实不足），
导致服务端段错误退出。该用例被放在最后执行；服务端一旦退出，后续用例
会自动标记为「跳过」。
"""

import argparse
import json
import socket
import struct
import sys
import threading
import time

# ----------------------------- 协议常量 -----------------------------
MAGIC = 0xEB90
HEADER_FMT = "!HHI"          # magic(H) msgId(H) bodyLen(I)，网络字节序
HEADER_SIZE = struct.calcsize(HEADER_FMT)   # = 8
MAX_BODY_LEN = 1024 * 1024   # 服务端限制：包体不能超过 1MB


# ----------------------------- 编解码工具 -----------------------------f
def pack_message(msg_id: int, body: bytes) -> bytes:
    """按协议打包一条完整消息。"""
    return struct.pack(HEADER_FMT, MAGIC, msg_id & 0xFFFF, len(body)) + body


def recv_exact(sock: socket.socket, n: int) -> bytes:
    """精确读取 n 个字节，连接提前关闭则抛异常。"""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("连接被对端关闭（提前 EOF）")
        buf.extend(chunk)
    return bytes(buf)


def recv_message(sock: socket.socket):
    """读取一条完整消息，返回 (magic, msg_id, body)。"""
    magic, msg_id, body_len = struct.unpack(HEADER_FMT, recv_exact(sock, HEADER_SIZE))
    body = recv_exact(sock, body_len) if body_len else b""
    return magic, msg_id, body


def connect(host: str, port: int, timeout: float = 5.0) -> socket.socket:
    """建立一条客户端连接。"""
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


def server_alive(host: str, port: int, timeout: float = 0.5) -> bool:
    """探测服务端是否仍然存活（用于崩溃后跳过后续用例）。"""
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


# ----------------------------- 测试框架 -----------------------------
class Tester:
    def __init__(self, host: str, port: int, verbose: bool = False):
        self.host = host
        self.port = port
        self.verbose = verbose
        self.passed = 0
        self.failed = 0
        self.skipped = 0

    def check(self, name: str, cond: bool, detail: str = ""):
        if cond:
            self.passed += 1
            print(f"  \033[32m[PASS]\033[0m {name}")
        else:
            self.failed += 1
            print(f"  \033[31m[FAIL]\033[0m {name}  {detail}")
        return cond

    def run(self, name, fn):
        print(f"\n\033[36m▶ {name}\033[0m")
        try:
            fn()
        except Exception as exc:  # noqa: BLE001
            self.failed += 1
            print(f"  \033[31m[ERROR]\033[0m {name} 抛出异常: {exc!r}")

    # ---------- 用例 ----------
    def test_basic_echo(self):
        """单条消息原样回显。"""
        with connect(self.host, self.port) as sock:
            body = json.dumps({"msg": "hello", "n": 1}).encode()
            sock.sendall(pack_message(1001, body))
            magic, msg_id, resp = recv_message(sock)
            self.check("魔数正确", magic == MAGIC, f"got 0x{magic:04X}")
            self.check("msgId 一致", msg_id == 1001, f"got {msg_id}")
            self.check("包体原样回显", resp == body, f"got {resp!r}")

    def test_utf8_body(self):
        """中文 UTF-8 包体。"""
        with connect(self.host, self.port) as sock:
            body = json.dumps({"昵称": "张三", "内容": "你好，回音壁！"},
                              ensure_ascii=False).encode("utf-8")
            sock.sendall(pack_message(7, body))
            _, msg_id, resp = recv_message(sock)
            self.check("msgId 一致", msg_id == 7)
            self.check("中文字节无损", resp == body, f"got {resp!r}")

    def test_empty_body(self):
        """空包体（bodyLen = 0）。"""
        with connect(self.host, self.port) as sock:
            sock.sendall(pack_message(0, b""))
            magic, msg_id, resp = recv_message(sock)
            self.check("空包可回显", magic == MAGIC and msg_id == 0 and resp == b"")

    def test_sequence(self):
        """连续 100 条消息，顺序与内容都不能乱。"""
        with connect(self.host, self.port) as sock:
            total = 100
            for i in range(total):
                sock.sendall(pack_message(i, f"seq-{i}".encode()))
            ok = True
            for i in range(total):
                _, msg_id, resp = recv_message(sock)
                if msg_id != i or resp != f"seq-{i}".encode():
                    ok = False
                    break
            self.check(f"连续 {total} 条顺序正确", ok)

    def test_sticky_packets(self):
        """粘包：一次 send 发送两条消息，应收到两条独立回显。"""
        with connect(self.host, self.port) as sock:
            sock.sendall(pack_message(11, b"aaa") + pack_message(22, b"bbb"))
            _, id1, body1 = recv_message(sock)
            _, id2, body2 = recv_message(sock)
            self.check("拆出第 1 条", (id1, body1) == (11, b"aaa"), f"got {(id1, body1)}")
            self.check("拆出第 2 条", (id2, body2) == (22, b"bbb"), f"got {(id2, body2)}")

    def test_half_packet(self):
        """半包：头与体分两次发送，中间有间隔。"""
        with connect(self.host, self.port) as sock:
            body = b"split-body-0123456789"
            packet = pack_message(33, body)
            sock.sendall(packet[:5])          # 只发半个头
            time.sleep(0.2)
            sock.sendall(packet[5:])          # 补发剩余
            _, msg_id, resp = recv_message(sock)
            self.check("半包可正确重组", (msg_id, resp) == (33, body), f"got {(msg_id, resp)}")

    def test_byte_by_byte(self):
        """逐字节发送，考验状态机边界。"""
        with connect(self.host, self.port) as sock:
            body = b"byte-stream"
            packet = pack_message(44, body)
            for b in packet:
                sock.sendall(bytes([b]))
                time.sleep(0.005)
            _, msg_id, resp = recv_message(sock)
            self.check("逐字节可正确重组", (msg_id, resp) == (44, body), f"got {(msg_id, resp)}")

    def test_large_body(self):
        """大包体（256KB，< 1MB 上限）。"""
        with connect(self.host, self.port) as sock:
            body = bytes(range(256)) * 1024          # 256KB
            sock.sendall(pack_message(55, body))
            _, msg_id, resp = recv_message(sock)
            self.check("大包体完整回显", msg_id == 55 and resp == body,
                       f"len={len(resp)} 期望 {len(body)}")

    def test_msgid_range(self):
        """msgId 上下边界（0 与 65535）。"""
        with connect(self.host, self.port) as sock:
            sock.sendall(pack_message(0, b"low") + pack_message(65535, b"high"))
            _, id1, b1 = recv_message(sock)
            _, id2, b2 = recv_message(sock)
            self.check("msgId=0 正确", (id1, b1) == (0, b"low"), f"got {(id1, b1)}")
            self.check("msgId=65535 正确", (id2, b2) == (65535, b"high"), f"got {(id2, b2)}")

    def test_invalid_magic(self):
        """非法魔数：服务端应关闭连接。"""
        sock = connect(self.host, self.port)
        try:
            bad = struct.pack(HEADER_FMT, 0x1234, 1, 0)
            sock.sendall(bad)
            closed = False
            try:
                sock.settimeout(2.0)
                closed = sock.recv(1) == b""
            except (ConnectionResetError, socket.timeout, OSError):
                closed = True
            self.check("非法魔数被断开", closed)
        finally:
            sock.close()

    def test_oversize_body(self):
        """包体声明超过 1MB：服务端应关闭连接。"""
        sock = connect(self.host, self.port)
        try:
            oversize = MAX_BODY_LEN + 1
            sock.sendall(struct.pack(HEADER_FMT, MAGIC, 1, oversize))
            closed = False
            try:
                sock.settimeout(2.0)
                closed = sock.recv(1) == b""
            except (ConnectionResetError, socket.timeout, OSError):
                closed = True
            self.check("超大包体被断开", closed)
        finally:
            sock.close()

    def test_concurrent_clients(self):
        """并发 20 个客户端，互不串包。"""
        n_clients = 20
        rounds = 10
        errors = []
        barrier = threading.Barrier(n_clients)

        def worker(cid: int):
            try:
                with connect(self.host, self.port) as sock:
                    barrier.wait(timeout=10)
                    for r in range(rounds):
                        body = f"client-{cid}-round-{r}".encode()
                        sock.sendall(pack_message(cid, body))
                        _, msg_id, resp = recv_message(sock)
                        if msg_id != cid or resp != body:
                            errors.append(f"client {cid}: got {(msg_id, resp)!r}")
            except Exception as exc:  # noqa: BLE001
                errors.append(f"client {cid}: {exc!r}")

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(n_clients)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=20)

        self.check(f"{n_clients} 并发客户端 × {rounds} 轮无串包",
                   not errors, "; ".join(errors[:3]))

    def test_reconnect(self):
        """短连接反复建连/断开，验证服务端资源回收。"""
        ok = True
        try:
            for i in range(30):
                with connect(self.host, self.port) as sock:
                    body = f"conn-{i}".encode()
                    sock.sendall(pack_message(i, body))
                    _, msg_id, resp = recv_message(sock)
                    if (msg_id, resp) != (i, body):
                        ok = False
                        break
        except Exception:  # noqa: BLE001
            ok = False
        self.check("30 次短连接稳定", ok)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="回音壁(Echo Server)协议测试脚本",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("-H", "--host", default="127.0.0.1", help="服务器地址")
    parser.add_argument("-p", "--port", type=int, default=8888, help="服务器端口")
    args = parser.parse_args()

    print("=" * 60)
    print(f"  回音壁协议测试  →  {args.host}:{args.port}")
    print("=" * 60)

    # 先做一次连通性检查，避免一串无意义报错
    try:
        with connect(args.host, args.port, timeout=3.0):
            pass
    except OSError as exc:
        print(f"\n\033[31m无法连接 {args.host}:{args.port} —— 请确认服务端已启动。\033[0m")
        print(f"错误详情: {exc}")
        return 1

    t = Tester(args.host, args.port)
    # 顺序说明：把"大包体"放在最后，因为它会触发服务端 Buffer::append 的越界写，
    # 导致服务端进程崩溃；崩溃后剩余用例会被自动跳过，避免误导性的级联失败。
    cases = [
        ("1. 基本回显", t.test_basic_echo),
        ("2. 中文 UTF-8 包体", t.test_utf8_body),
        ("3. 空包体", t.test_empty_body),
        ("4. 连续 100 条消息", t.test_sequence),
        ("5. 粘包（两条合并发送）", t.test_sticky_packets),
        ("6. 半包（分片发送）", t.test_half_packet),
        ("7. 逐字节发送", t.test_byte_by_byte),
        ("8. msgId 边界值", t.test_msgid_range),
        ("9. 非法魔数应断开", t.test_invalid_magic),
        ("10. 超大包体应断开", t.test_oversize_body),
        ("11. 并发 20 客户端", t.test_concurrent_clients),
        ("12. 短连接重连", t.test_reconnect),
        ("13. 大包体 256KB [已知会压垮服务端]", t.test_large_body),
    ]
    for name, fn in cases:
        if not server_alive(args.host, args.port):
            t.skipped += 1
            print(f"\n\033[33m⏭ {name}  —  跳过（服务端已不可用，可能已被上一个用例压垮）\033[0m")
            continue
        t.run(name, fn)

    total = t.passed + t.failed
    print("\n" + "=" * 60)
    color = "\033[32m" if t.failed == 0 else "\033[31m"
    line = f"  测试结果: {t.passed}/{total} 通过, {t.failed} 失败"
    if t.skipped:
        line += f", {t.skipped} 跳过"
    print(f"{color}{line}\033[0m")
    print("=" * 60)
    return 0 if t.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
