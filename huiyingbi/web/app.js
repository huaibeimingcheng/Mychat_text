/* 聊天室前端：所有操作均通过点击/表单完成 */
(() => {
  'use strict';

  // ---------------- DOM ----------------
  const $ = (id) => document.getElementById(id);
  const statusDot = $('statusDot');
  const statusText = $('statusText');
  const authPanel = $('authPanel');
  const chatPanel = $('chatPanel');
  const tabLogin = $('tabLogin');
  const tabRegister = $('tabRegister');
  const authForm = $('authForm');
  const authSubmit = $('authSubmit');
  const authHint = $('authHint');
  const usernameInput = $('username');
  const passwordInput = $('password');
  const userListEl = $('userList');
  const onlineBadge = $('onlineBadge');
  const messagesEl = $('messages');
  const meNameEl = $('meName');
  const chatForm = $('chatForm');
  const messageInput = $('messageInput');
  const btnLogout = $('btnLogout');
  const closeNotice = $('closeNotice');

  // ---------------- 状态 ----------------
  let ws = null;
  let mode = 'login';           // login | register
  let loggedIn = false;
  let me = '';
  let reconnectDelay = 1000;
  let reconnectTimer = null;
  const MAX_RECONNECT_DELAY = 15000;

  // ---------------- 工具 ----------------
  function setStatus(text, cls) {
    statusText.textContent = text;
    statusDot.className = 'dot' + (cls ? ' ' + cls : '');
  }

  function setHint(text, cls) {
    authHint.textContent = text || '';
    authHint.className = 'hint' + (cls ? ' ' + cls : '');
  }

  function nowTime() {
    const d = new Date();
    const p = (n) => String(n).padStart(2, '0');
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
  }

  function scrollToBottom() {
    messagesEl.scrollTop = messagesEl.scrollHeight;
  }

  // 全部使用 textContent，杜绝 XSS
  function addSystem(text, time) {
    const div = document.createElement('div');
    div.className = 'sys';
    div.textContent = text;
    if (time) {
      const t = document.createElement('span');
      t.className = 't';
      t.textContent = time;
      div.appendChild(t);
    }
    messagesEl.appendChild(div);
    scrollToBottom();
  }

  function addError(text) {
    const div = document.createElement('div');
    div.className = 'errMsg';
    div.textContent = '⚠ ' + text;
    messagesEl.appendChild(div);
    scrollToBottom();
  }

  function addChat(from, text, time, isSelf) {
    const wrap = document.createElement('div');
    wrap.className = 'msg ' + (isSelf ? 'self' : 'other');

    const meta = document.createElement('div');
    meta.className = 'meta';
    meta.textContent = `${from} · ${time || nowTime()}`;

    const bubble = document.createElement('div');
    bubble.className = 'bubble';
    bubble.textContent = text;

    wrap.appendChild(meta);
    wrap.appendChild(bubble);
    messagesEl.appendChild(wrap);
    scrollToBottom();
  }

  function renderUserList(users) {
    userListEl.innerHTML = '';
    (users || []).forEach((u) => {
      const li = document.createElement('li');
      li.textContent = u === me ? u + '（我）' : u;
      li.title = '点击 @ ' + u;
      li.addEventListener('click', () => {
        messageInput.value = (messageInput.value ? messageInput.value + ' ' : '') + '@' + u + ' ';
        messageInput.focus();
      });
      userListEl.appendChild(li);
    });
    onlineBadge.textContent = `在线 ${(users || []).length} 人`;
  }

  // ---------------- WebSocket ----------------
  function wsUrl() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    return `${proto}//${location.host}/ws`;
  }

  function connect() {
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;

    setStatus('连接中…', 'connecting');
    try {
      ws = new WebSocket(wsUrl());
    } catch (e) {
      setStatus('连接失败', '');
      scheduleReconnect();
      return;
    }

    ws.onopen = () => {
      reconnectDelay = 1000;
      setStatus('已连接', 'online');
      setHint('');
    };

    ws.onmessage = (ev) => {
      let msg;
      try { msg = JSON.parse(ev.data); } catch (_) { return; }
      handleMessage(msg);
    };

    ws.onclose = () => {
      setStatus('已断开', '');
      if (loggedIn) {
        addError('与服务器断开连接，正在尝试重连…');
        loggedIn = false;
        showAuth();
      }
      scheduleReconnect();
    };

    ws.onerror = () => { /* 具体原因交给 onclose 处理 */ };
  }

  function scheduleReconnect() {
    if (reconnectTimer) return;
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      connect();
    }, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 1.6, MAX_RECONNECT_DELAY);
  }

  function send(obj) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      addError('尚未连接到服务器，请稍候重试');
      return false;
    }
    ws.send(JSON.stringify(obj));
    return true;
  }

  // ---------------- 消息处理 ----------------
  function handleMessage(msg) {
    switch (msg.type) {
      case 'system':
        addSystem(msg.text, msg.time);
        if (msg.needLogin) setHint(msg.text + '（服务器不保存聊天记录）', '');
        break;

      case 'register_result':
        if (msg.ok) {
          switchMode('login');          // 先切页签（内部会清空提示）
          setHint(msg.message || '注册成功，请登录', 'ok');  // 再写提示，避免被覆盖
          usernameInput.value = msg.username || usernameInput.value;
          passwordInput.value = '';
          passwordInput.focus();
        } else {
          setHint(msg.reason || '注册失败', 'err');
        }
        setBusy(false);
        break;

      case 'login_result':
        setBusy(false);
        if (msg.ok) {
          loggedIn = true;
          me = msg.username;
          meNameEl.textContent = me;
          authPanel.classList.add('hidden');
          chatPanel.classList.remove('hidden');
          messagesEl.innerHTML = '';
          const notice = document.createElement('div');
          notice.className = 'notice';
          notice.innerHTML = '聊天室<b>不提供历史消息</b>：这里只会显示你进入之后的消息。';
          messagesEl.appendChild(notice);
          renderUserList(msg.users || []);
          addSystem(msg.message || '已进入聊天室', nowTime());
          messageInput.focus();
          setHint('');
        } else {
          setHint(msg.reason || '登录失败', 'err');
        }
        break;

      case 'chat':
        addChat(msg.from, msg.text, msg.time, msg.from === me);
        break;

      case 'user_list':
        renderUserList(msg.users || []);
        break;

      case 'logout_result':
        if (msg.ok) {
          loggedIn = false;
          me = '';
          showAuth();
          setHint(msg.message || '已退出登录', 'ok');
        }
        break;

      case 'error':
        addError(msg.reason || '未知错误');
        break;

      default:
        break;
    }
  }

  // ---------------- 界面切换 ----------------
  function showAuth() {
    chatPanel.classList.add('hidden');
    authPanel.classList.remove('hidden');
    passwordInput.value = '';
    setBusy(false);
  }

  function switchMode(next) {
    mode = next;
    const isLogin = mode === 'login';
    tabLogin.classList.toggle('active', isLogin);
    tabRegister.classList.toggle('active', !isLogin);
    authSubmit.textContent = isLogin ? '登录' : '注册';
    setHint(isLogin ? '' : '密码 6~64 字节，用户名 2~16 字节');
  }

  function setBusy(busy) {
    authSubmit.disabled = busy;
    authSubmit.textContent = busy ? '处理中…' : (mode === 'login' ? '登录' : '注册');
  }

  // ---------------- 事件绑定 ----------------
  tabLogin.addEventListener('click', () => switchMode('login'));
  tabRegister.addEventListener('click', () => switchMode('register'));

  authForm.addEventListener('submit', (e) => {
    e.preventDefault();
    const username = usernameInput.value.trim();
    const password = passwordInput.value;
    if (!username) { setHint('请输入用户名', 'err'); return; }
    if (!password) { setHint('请输入密码', 'err'); return; }

    if (!ws || ws.readyState !== WebSocket.OPEN) {
      setHint('尚未连接到服务器，请稍候…', 'err');
      connect();
      return;
    }

    setBusy(true);
    setHint('');
    if (mode === 'register') {
      send({ type: 'register', username, password });
    } else {
      send({ type: 'login', username, password });
    }
    // 3 秒没响应则恢复按钮，避免"卡死"观感
    setTimeout(() => setBusy(false), 3000);
  });

  chatForm.addEventListener('submit', (e) => {
    e.preventDefault();
    const text = messageInput.value.trim();
    if (!text) return;
    if (!send({ type: 'chat', text })) return;
    messageInput.value = '';
    messageInput.style.height = 'auto';
  });

  messageInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      chatForm.requestSubmit();
    }
  });

  messageInput.addEventListener('input', () => {
    messageInput.style.height = 'auto';
    messageInput.style.height = Math.min(messageInput.scrollHeight, 140) + 'px';
  });

  btnLogout.addEventListener('click', () => {
    if (loggedIn) send({ type: 'logout' });
  });

  if (closeNotice) {
    closeNotice.addEventListener('click', () => {
      const n = closeNotice.closest('.notice');
      if (n) n.remove();
    });
  }

  // ---------------- 启动 ----------------
  switchMode('login');
  connect();
})();
