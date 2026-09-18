"""只监听回环的 SSH 协议夹具；返回固定输出，绝不执行真实 shell 命令。"""
from __future__ import annotations
import os
from pathlib import Path
import site
import socket
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
site.addsitedir(str(ROOT / "build/mcp-test-deps"))
import paramiko


class Server(paramiko.ServerInterface):
    def check_auth_password(self, username, password):
        return paramiko.AUTH_SUCCESSFUL if username == "fixture" and password == "fixture-only" else paramiko.AUTH_FAILED

    def get_allowed_auths(self, username):
        return "password"

    def check_channel_request(self, kind, chanid):
        return paramiko.OPEN_SUCCEEDED if kind == "session" else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_pty_request(self, channel, term, width, height, pixelwidth, pixelheight, modes):
        return True

    def check_channel_shell_request(self, channel):
        return True

    def check_channel_exec_request(self, channel, command):
        def reply():
            try:
                # 让 Paramiko 先发送 exec 成功应答，再发布这次命令的数据和退出状态。
                time.sleep(0.02)
                if command == b"fixture-ok":
                    channel.sendall(b"ok\n")
                    channel.send_exit_status(0)
                elif command == b"fixture-nonzero":
                    channel.sendall_stderr(b"expected error\n")
                    channel.send_exit_status(42)
                elif command == b"fixture-limit":
                    channel.sendall(b"x" * 1024)
                    channel.send_exit_status(0)
                elif command == b"fixture-timeout":
                    return
                else:
                    channel.send_exit_status(127)
                channel.close()
            except (OSError, EOFError):
                pass
        threading.Thread(target=reply, daemon=True).start()
        return True


def main():
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/Release/bin/novaterm_mcp_tests.exe")
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(10)
    transports = []
    stop = threading.Event()
    host_key = paramiko.RSAKey.generate(2048)

    def serve():
        try:
            client, _ = listener.accept()
            transport = paramiko.Transport(client)
            transports.append(transport)
            transport.add_server_key(host_key)
            transport.start_server(server=Server())
            channels = []
            while transport.is_active() and not stop.is_set():
                channel = transport.accept(0.1)
                if channel is not None:
                    channels.append(channel)
        except (OSError, EOFError):
            pass

    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    try:
        result = subprocess.run([str(executable), "--ssh-loopback-check", str(listener.getsockname()[1])],
            timeout=30, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        print(result.stdout.decode("utf-8", errors="replace"), end="")
        if result.returncode:
            print(result.stderr.decode("utf-8", errors="replace"), file=sys.stderr, end="")
            print(f"SSH fixture process exit: {result.returncode:#x}", file=sys.stderr)
        if result.returncode:
            raise SystemExit(result.returncode)
    finally:
        stop.set()
        listener.close()
        for transport in transports:
            transport.close()
        worker.join(timeout=3)


if __name__ == "__main__":
    main()
