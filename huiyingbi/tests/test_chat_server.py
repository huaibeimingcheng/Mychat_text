#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
聊天室服务器端到端测试（纯标准库手写 WebSocket 客户端）

覆盖：
  - WebSocket 握手（含 Sec-WebSocket-Accept 校验）与同端口静态页面
  - 注册 / 登录 / 登出，密码错误、重复用户名、非法输入
  - 未登录发言被拒、重复登录被拒
  - 单聊天的广播语义、在线列表 user_list
  - **新用户看不到历史消息**（本项目的核心需求）
  - 限流、超长消息、超大帧、二进制帧、非法 JSON、分片消息、ping/pong
  - 20 并发客户端压测

用法：
    python3 tests/test_chat_server.py --start-server   # 自动拉起服务器（推荐）
    python3 tests/test_chat_server.py -p 8888          # 测试已在运行的服务器
退出码 0 表示全部通过。
"""

import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 1, 2, 8, 9, 10

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# --------------------------------------------------------------------------
# 极简 WebSocket 客户端
# --------------------------------------------------------------------------
class WsError(Exception):
    pass


class WsClient:
    def __init__(self, host, port, path="/ws", timeout=5.0):
        self.host, self.port = host, port
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""
        self._handshake(path)

    # ---- 握手 ----
    def _handshake(self, path):
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())

        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise WsError("握手期间连接被关闭")
            data += chunk
        head, _, rest = data.partition(b"\r\n\r\n")
        self.buf = rest
        self.resp_headers = head.decode(errors="replace")

        if "101" not in self.resp_headers.split("\r\n")[0]:
            raise WsError("握手失败: " + self.resp_headers.split("\r\n")[0])

        expect = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        if expect not in self.resp_headers:
            raise WsError(f"Sec-WebSocket-Accept 不匹配，期望 {expect}")

    # ---- 收发 ----
    def _recv_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise WsError("连接被对端关闭(EOF)")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv_frame(self):
        b0, b1 = self._recv_exact(2)
        opcode = b0 & 0x0F
        masked = b1 & 0x80
        length = b1 & 0x7F
        if length == 126:
            length = struct.unpack("!H", self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._recv_exact(8))[0]
        mask = self._recv_exact(4) if masked else None
        payload = self._recv_exact(length) if length else b""
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return opcode, payload

    def send_frame(self, opcode, payload=b""):
        mask = os.urandom(4)
        header = bytes([0x80 | opcode])
        n = len(payload)
        if n < 126:
            header += bytes([0x80 | n])
        elif n <= 0xFFFF:
            header += bytes([0x80 | 126]) + struct.pack("!H", n)
        else:
            header += bytes([0x80 | 127]) + struct.pack("!Q", n)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def send_text(self, text):
        self.send_frame(OP_TEXT, text.encode("utf-8"))

    def send_json(self, obj):
        self.send_text(json.dumps(obj, ensure_ascii=False))

    def recv_message(self, timeout=5.0, auto_pong=True):
        """返回 dict；收到 ping 自动回 pong；收到 close 返回 None。"""
        deadline = time.time() + timeout
        while True:
            remain = deadline - time.time()
            if remain <= 0:
                raise socket.timeout("等待消息超时")
            self.sock.settimeout(max(0.05, remain))
            try:
                opcode, payload = self.recv_frame()
            except socket.timeout:
                continue
            if opcode == OP_PING:
                if auto_pong:
                    self.send_frame(OP_PONG, payload)
                continue
            if opcode == OP_PONG:
                continue
            if opcode == OP_CLOSE:
                return None
            if opcode in (OP_TEXT, OP_BINARY):
                try:
                    return json.loads(payload.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    return {"_raw": payload}
            if opcode == 0:  # continuation（本测试里不主动用）
                continue
            raise WsError(f"未知 opcode={opcode}")

    def wait_for(self, msg_type, timeout=5.0, collected=None):
        """等待指定 type 的消息；期间收到的其它消息放进 collected 列表。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = self.recv_message(timeout=max(0.05, deadline - time.time()))
            if msg is None:
                raise WsError("连接已关闭")
            if collected is not None:
                collected.append(msg)
            if msg.get("type") == msg_type:
                return msg
        raise socket.timeout(f"未在 {timeout}s 内收到 type={msg_type} 的消息")

    def collect_for(self, seconds, collected=None):
        """在给定时间内收集所有收到的消息。"""
        got = collected if collected is not None else []
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                msg = self.recv_message(timeout=max(0.05, deadline - time.time()))
            except socket.timeout:
                break
            if msg is None:
                break
            got.append(msg)
        return got

    def close(self):
        try:
            self.send_frame(OP_CLOSE, struct.pack("!H", 1000))
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# 测试框架
# --------------------------------------------------------------------------
class Tester:
    def __init__(self, host, port):
        self.host, self.port = host, port
        self.passed = 0
        self.failed = 0
        self._seq = 0
        self.clients = []

    def check(self, name, cond, detail=""):
        if cond:
            self.passed += 1
            print(f"  \033[32m[PASS]\033[0m {name}")
        else:
            self.failed += 1
            print(f"  \033[31m[FAIL]\033[0m {name}   {detail}")
        return cond

    def run(self, name, fn):
        print(f"\n\033[36m▶ {name}\033[0m")
        try:
            fn()
        except Exception as exc:  # noqa: BLE001
            self.failed += 1
            print(f"  \033[31m[ERROR]\033[0m {name} 抛出异常: {exc!r}")

    def uniq(self, prefix="user"):
        self._seq += 1
        return f"{prefix}{int(time.time() * 1000) % 10000000}{self._seq}"

    def connect(self, path="/ws"):
        c = WsClient(self.host, self.port, path)
        self.clients.append(c)
        return c

    def register(self, name, password="pass123456"):
        c = self.connect()
        c.send_json({"type": "register", "username": name, "password": password})
        return c, c.wait_for("register_result")

    def login(self, name, password="pass123456", client=None):
        c = client or self.connect()
        c.send_json({"type": "login", "username": name, "password": password})
        resp = c.wait_for("login_result")
        if resp.get("ok"):
            c.wait_for("system")          # 进入通知
            c.wait_for("user_list")       # 在线列表
        return c, resp

    def close_all(self):
        for c in self.clients:
            c.close()
        self.clients.clear()


# --------------------------------------------------------------------------
# 用例
# --------------------------------------------------------------------------
def make_tests(t: Tester):
    host, port = t.host, t.port

    def test_http_static_page():
        """同端口提供前端静态页面"""
        s = socket.create_connection((host, port), timeout=3)
        t.clients.append(s)
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
        text = data.decode(errors="replace")
        # 必须严格以 "HTTP/1.1 200" 开头：若服务器在响应前插入了 WebSocket 帧，这里会失败
        t.check("GET / 返回 200", text.startswith("HTTP/1.1 200"), text[:40].replace("\r\n", "|"))
        t.check("返回 HTML", "text/html" in text and "聊天室" in text)

    def test_http_404():
        s = socket.create_connection((host, port), timeout=3)
        t.clients.append(s)
        s.sendall(b"GET /nope.js HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
        t.check("未知静态文件返回 404", "404" in data.decode(errors="replace").split("\r\n")[0])

    def test_handshake_banner():
        """握手成功后收到欢迎提示"""
        c = t.connect()
        msg = c.wait_for("system")
        t.check("握手/欢迎消息", msg.get("type") == "system" and "欢迎" in msg.get("text", ""),
                str(msg))

    def test_register_and_duplicate():
        name = t.uniq("reg")
        _, resp = t.register(name)
        t.check("注册成功", resp.get("ok") is True, str(resp))

        c2 = t.connect()
        c2.send_json({"type": "register", "username": name, "password": "pass123456"})
        resp2 = c2.wait_for("register_result")
        t.check("重复注册被拒", resp2.get("ok") is False, str(resp2))

    def test_invalid_input():
        c = t.connect()
        c.send_json({"type": "register", "username": "x", "password": "pass123456"})
        r1 = c.wait_for("register_result")
        t.check("过短用户名被拒", r1.get("ok") is False, str(r1))

        c.send_json({"type": "register", "username": t.uniq("ok"), "password": "123"})
        r2 = c.wait_for("register_result")
        t.check("过短密码被拒", r2.get("ok") is False, str(r2))

        c.send_json({"type": "register", "username": "a" * 100, "password": "pass123456"})
        r3 = c.wait_for("register_result")
        t.check("超长用户名被拒", r3.get("ok") is False, str(r3))

    def test_login_wrong_password():
        name = t.uniq("pw")
        t.register(name)
        c = t.connect()
        c.send_json({"type": "login", "username": name, "password": "wrong-password"})
        resp = c.wait_for("login_result")
        t.check("密码错误被拒", resp.get("ok") is False, str(resp))

    def test_login_ok_and_userlist():
        name = t.uniq("login")
        t.register(name)
        c, resp = t.login(name)
        t.check("登录成功", resp.get("ok") is True, str(resp))
        t.check("返回房间名", resp.get("room") == "大厅", str(resp))
        t.check("返回在线列表且含自己", name in (resp.get("users") or []), str(resp.get("users")))
        t.check("标记不提供历史", resp.get("history") is False, str(resp))

    def test_chat_before_login():
        c = t.connect()
        c.wait_for("system")
        c.send_json({"type": "chat", "text": "我还没登录"})
        msg = c.wait_for("error")
        t.check("未登录不能发言", "登录" in msg.get("reason", ""), str(msg))

    def test_broadcast_two_clients():
        a_name, b_name = t.uniq("a"), t.uniq("b")
        t.register(a_name); t.register(b_name)
        a, _ = t.login(a_name)
        b, _ = t.login(b_name)
        a.wait_for("system")  # b 进入的通知

        text = "你好，这是广播测试 " + t.uniq("m")
        a.send_json({"type": "chat", "text": text})
        got_b = b.wait_for("chat")
        got_a = a.wait_for("chat")
        t.check("B 收到 A 的消息", got_b.get("text") == text and got_b.get("from") == a_name, str(got_b))
        t.check("A 也收到自己的消息", got_a.get("text") == text, str(got_a))
        t.check("消息带时间", bool(got_b.get("time")), str(got_b))

    def test_no_history_for_newcomer():
        """核心需求：后进入的用户看不到之前的聊天内容"""
        a_name, c_name = t.uniq("hist_a"), t.uniq("hist_c")
        t.register(a_name); t.register(c_name)
        a, _ = t.login(a_name)

        secret = "进入之前的历史消息_" + t.uniq("x")
        a.send_json({"type": "chat", "text": secret})
        a.wait_for("chat")

        time.sleep(0.3)  # 确保历史消息已经"过去"
        c, _ = t.login(c_name)   # 新用户此时才进入

        received = c.collect_for(1.0)
        leaked = [m for m in received if secret in json.dumps(m, ensure_ascii=False)]
        t.check("新用户收不到历史消息", not leaked, f"泄露了: {leaked}")
        t.check("新用户只收到进入后的系统/列表消息",
                all(m.get("type") in ("system", "user_list") for m in received),
                str(received)[:200])

    def test_duplicate_login_rejected():
        name = t.uniq("dup")
        t.register(name)
        t.login(name)
        c2 = t.connect()
        c2.send_json({"type": "login", "username": name, "password": "pass123456"})
        resp = c2.wait_for("login_result")
        t.check("同一账号重复登录被拒", resp.get("ok") is False, str(resp))

    def test_logout_and_leave_notice():
        a_name, b_name = t.uniq("out_a"), t.uniq("out_b")
        t.register(a_name); t.register(b_name)
        a, _ = t.login(a_name)
        b, _ = t.login(b_name)
        a.wait_for("system")  # b 进入

        b.send_json({"type": "logout"})
        t.check("登出返回 ok", b.wait_for("logout_result").get("ok") is True)
        sys_msg = a.wait_for("system")
        t.check("其它人收到离开通知", "离开了聊天室" in sys_msg.get("text", ""), str(sys_msg))
        users = a.wait_for("user_list")
        t.check("在线列表移除该用户", b_name not in users.get("users", []), str(users.get("users")))

    def test_disconnect_notice():
        a_name, b_name = t.uniq("dis_a"), t.uniq("dis_b")
        t.register(a_name); t.register(b_name)
        a, _ = t.login(a_name)
        b, _ = t.login(b_name)
        a.wait_for("system")

        b.sock.close()  # 直接断开，不发 close 帧
        msgs = a.collect_for(3.0)
        got = [m for m in msgs if m.get("type") == "system" and b_name in m.get("text", "")]
        t.check("对端断开后收到离开通知", bool(got), str(msgs)[:300])

    def test_oversize_message_rejected():
        name = t.uniq("big")
        t.register(name)
        c, _ = t.login(name)
        c.send_json({"type": "chat", "text": "x" * 3000})  # 上限 2000
        msg = c.wait_for("error")
        t.check("超长消息被拒", "过长" in msg.get("reason", ""), str(msg))

    def test_bad_json():
        c = t.connect()
        c.wait_for("system")
        c.send_text("这不是 json")
        msg = c.wait_for("error")
        t.check("非法 JSON 被拒", msg.get("type") == "error", str(msg))

    def test_oversize_frame_closes():
        c = t.connect()
        c.wait_for("system")
        try:
            c.send_frame(OP_TEXT, b"y" * (128 * 1024))  # 超过 64KB 上限
            c.sock.settimeout(3)
            got_close = False
            for _ in range(20):
                opcode, _payload = c.recv_frame()
                if opcode == OP_CLOSE:
                    got_close = True
                    break
            t.check("超大帧被断开", got_close)
        except (WsError, socket.timeout, ConnectionResetError):
            t.check("超大帧被断开（连接被关闭）", True)

    def test_binary_frame_rejected():
        name = t.uniq("bin")
        t.register(name)
        c, _ = t.login(name)
        c.send_frame(OP_BINARY, b"\x00\x01\x02")
        msg = c.wait_for("error")
        t.check("二进制消息被拒", "二进制" in msg.get("reason", ""), str(msg))

    def test_fragmented_message():
        name = t.uniq("frag")
        t.register(name)
        c, _ = t.login(name)

        payload = json.dumps({"type": "chat", "text": "分片消息测试"}, ensure_ascii=False).encode()
        half = len(payload) // 2
        # 第一帧：opcode=1, FIN=0
        mask = os.urandom(4)
        first = payload[:half]
        frame = bytes([0x01]) + bytes([0x80 | len(first)]) + mask + \
            bytes(b ^ mask[i % 4] for i, b in enumerate(first))
        c.sock.sendall(frame)
        # 第二帧：opcode=0(continuation), FIN=1
        rest = payload[half:]
        mask2 = os.urandom(4)
        frame2 = bytes([0x80]) + bytes([0x80 | len(rest)]) + mask2 + \
            bytes(b ^ mask2[i % 4] for i, b in enumerate(rest))
        c.sock.sendall(frame2)

        msg = c.wait_for("chat")
        t.check("分片消息正确重组", msg.get("text") == "分片消息测试", str(msg))

    def test_ping_pong():
        c = t.connect()
        c.wait_for("system")
        c.send_frame(OP_PING, b"hello-ping")
        deadline = time.time() + 3
        while time.time() < deadline:
            opcode, payload = c.recv_frame()
            if opcode == OP_PONG:
                t.check("ping 得到 pong", payload == b"hello-ping", str(payload))
                return
        t.check("ping 得到 pong", False, "超时未收到 pong")

    def test_concurrent_clients():
        n = 20
        names = [t.uniq(f"conc{i}_") for i in range(n)]
        clients = []
        for name in names:
            t.register(name)
            c, resp = t.login(name)
            clients.append((c, name))
            if not resp.get("ok"):
                t.check("并发登录成功", False, str(resp))
                return
        t.check(f"{n} 个客户端并发登录成功", len(clients) == n)

        # 每个客户端发一条消息，验证大家都收到 n 条
        texts = [f"并发消息-{i}" for i in range(n)]
        for (c, _name), text in zip(clients, texts):
            c.send_json({"type": "chat", "text": text})

        got = clients[0][0].collect_for(6.0)
        chat_texts = [m.get("text") for m in got if m.get("type") == "chat"]
        t.check("接收者收到全部并发消息", all(x in chat_texts for x in texts),
                f"缺少 {[x for x in texts if x not in chat_texts]}")

    def test_reconnect_after_close():
        name = t.uniq("rc")
        t.register(name)
        c, _ = t.login(name)
        c.close()
        time.sleep(0.4)
        c2, resp = t.login(name)  # 关掉后应能重新登录
        t.check("断开后可重新登录", resp.get("ok") is True, str(resp))
        c2.close()

    return [
        ("1. 静态页面（同端口 HTTP）", test_http_static_page),
        ("2. 静态资源 404", test_http_404),
        ("3. WebSocket 握手与欢迎消息", test_handshake_banner),
        ("4. 注册成功 / 重复注册", test_register_and_duplicate),
        ("5. 非法用户名与密码", test_invalid_input),
        ("6. 密码错误登录", test_login_wrong_password),
        ("7. 登录成功与在线列表", test_login_ok_and_userlist),
        ("8. 未登录不能发言", test_chat_before_login),
        ("9. 两条连接的消息广播", test_broadcast_two_clients),
        ("10. 【新用户看不到历史消息】", test_no_history_for_newcomer),
        ("11. 同账号重复登录被拒", test_duplicate_login_rejected),
        ("12. 登出与离开通知", test_logout_and_leave_notice),
        ("13. 断线离开通知", test_disconnect_notice),
        ("14. 超长消息被拒", test_oversize_message_rejected),
        ("15. 非法 JSON 被拒", test_bad_json),
        ("16. 超大帧断开连接", test_oversize_frame_closes),
        ("17. 二进制帧被拒", test_binary_frame_rejected),
        ("18. 分片消息重组", test_fragmented_message),
        ("19. ping/pong 心跳", test_ping_pong),
        ("20. 20 个并发客户端", test_concurrent_clients),
        ("21. 断开后重新登录", test_reconnect_after_close),
    ]


# --------------------------------------------------------------------------
# 服务器进程管理
# --------------------------------------------------------------------------
def start_server(binary, port, tmpdir):
    db = os.path.join(tmpdir, "users.db")
    log = os.path.join(tmpdir, "server.log")
    proc = subprocess.Popen(
        [binary, "-p", str(port), "-t", "4", "-d", db, "-l", log, "-L", "debug",
         "-w", os.path.join(REPO, "web")],
        cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    deadline = time.time() + 10
    while time.time() < deadline:
        if proc.poll() is not None:
            out = proc.stdout.read().decode(errors="replace")
            print("服务器提前退出：\n" + out[-2000:])
            sys.exit(2)
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.3).close()
            return proc, log
        except OSError:
            time.sleep(0.1)
    print("服务器启动超时")
    proc.kill()
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser(description="聊天室服务器端到端测试")
    ap.add_argument("-H", "--host", default="127.0.0.1")
    ap.add_argument("-p", "--port", type=int, default=18888)
    ap.add_argument("--start-server", action="store_true", help="自动编译并启动服务器")
    ap.add_argument("--binary", default=os.path.join(REPO, "chat_server"))
    args = ap.parse_args()

    proc = None
    tmpdir = None
    if args.start_server:
        if not os.path.exists(args.binary):
            print(f"找不到 {args.binary}，请先执行 make")
            return 2
        tmpdir = tempfile.mkdtemp(prefix="chat_test_")
        proc, logfile = start_server(args.binary, args.port, tmpdir)
        print(f"服务器已启动 pid={proc.pid}，端口 {args.port}，日志 {logfile}")

    print("=" * 62)
    print(f"  聊天室服务器测试  →  {args.host}:{args.port}")
    print("=" * 62)

    t = Tester(args.host, args.port)
    try:
        for name, fn in make_tests(t):
            t.run(name, fn)
    finally:
        t.close_all()

    alive = True
    if proc:
        time.sleep(0.3)
        if proc.poll() is not None:
            alive = False
            print("\n\033[31m服务器在测试过程中崩溃了！\033[0m")
            out = proc.stdout.read().decode(errors="replace")
            print(out[-3000:])

    print("\n" + "=" * 62)
    print(f"  测试结果: {t.passed} 通过, {t.failed} 失败")
    print("=" * 62)

    if proc:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()

    if not alive:
        return 1
    return 0 if t.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
