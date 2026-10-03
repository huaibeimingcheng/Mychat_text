#!/usr/bin/env bash
# 一键部署脚本（在云服务器上、项目根目录执行）
#   bash deploy/deploy.sh              # 编译并前台启动
#   bash deploy/deploy.sh -p 80        # 指定端口
#   bash deploy/deploy.sh --install-systemd <路径>   # 安装为开机自启服务
set -euo pipefail

PORT=8888
THREADS=4
LEVEL=info
BIND=0.0.0.0
INSTALL_SYSTEMD=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port)    PORT="$2"; shift 2 ;;
        -t|--threads) THREADS="$2"; shift 2 ;;
        -b|--bind)    BIND="$2"; shift 2 ;;
        -L|--level)   LEVEL="$2"; shift 2 ;;
        --install-systemd) INSTALL_SYSTEMD="$2"; shift 2 ;;
        -h|--help)
            echo "用法: bash deploy/deploy.sh [-p 端口] [-t 线程数] [-b 绑定地址] [-L 级别] [--install-systemd 安装路径]"
            exit 0 ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
echo "==> 项目目录: $ROOT"

# ---------- 1. 检查编译环境 ----------
if ! command -v g++ >/dev/null 2>&1; then
    echo "!! 未找到 g++，请先安装编译工具："
    echo "   CentOS/RHEL/openEuler: sudo yum install -y gcc-c++ make"
    echo "   Ubuntu/Debian:         sudo apt update && sudo apt install -y g++ make"
    exit 1
fi

GXX_VER="$(g++ -dumpversion | cut -d. -f1)"
if [[ "$GXX_VER" -lt 7 ]]; then
    echo "!! g++ 版本过低（当前 $(g++ -dumpversion)），本项目需要 C++17（GCC 7+）"
    echo "   CentOS 7 可安装 devtoolset："
    echo "     sudo yum install -y centos-release-scl && sudo yum install -y devtoolset-11-gcc-c++"
    echo "     scl enable devtoolset-11 bash    # 然后重新执行本脚本"
    exit 1
fi
echo "==> g++ $(g++ -dumpversion) 可用"

# ---------- 2. 编译 ----------
echo "==> 开始编译..."
make -j"$(nproc)"

# ---------- 3. 运行目录 ----------
mkdir -p data logs

LAUNCH_CMD="$ROOT/chat_server -b $BIND -p $PORT -t $THREADS -L $LEVEL"

# ---------- 4a. 安装为 systemd 服务 ----------
if [[ -n "$INSTALL_SYSTEMD" ]]; then
    TARGET="$INSTALL_SYSTEMD"
    echo "==> 安装到 $TARGET"
    sudo mkdir -p "$TARGET"
    sudo cp -r "$ROOT"/* "$TARGET"/
    sudo mkdir -p "$TARGET/data" "$TARGET/logs"

    UNIT=/etc/systemd/system/chat_server.service
    sudo sed "s#WorkingDirectory=.*#WorkingDirectory=$TARGET#; \
              s#ExecStart=.*#ExecStart=$TARGET/chat_server -b $BIND -p $PORT -t $THREADS -L $LEVEL#" \
        deploy/chat_server.service | sudo tee "$UNIT" >/dev/null

    sudo systemctl daemon-reload
    sudo systemctl enable --now chat_server
    echo "==> 已启动，查看状态：sudo systemctl status chat_server"
    echo "==> 跟日志：        sudo journalctl -u chat_server -f"
    echo "==> 程序日志：      tail -f $TARGET/logs/server.log"
    exit 0
fi

# ---------- 4b. 直接启动 ----------
echo
echo "=========================================="
echo " 启动命令（前台，Ctrl+C 停止）:"
echo "   $LAUNCH_CMD"
echo "=========================================="
echo " 后台运行请用:  nohup $LAUNCH_CMD > /dev/null 2>&1 &"
echo " 日志:          tail -f logs/server.log"
echo " 停止:          pkill -f chat_server"
echo "=========================================="
echo
exec $LAUNCH_CMD
