"""只监听回环的 SSH 协议夹具；返回固定输出，绝不执行真实 shell 命令。"""
from __future__ import annotations
import os
from pathlib import Path
import posixpath
import re
import site
import socket
import subprocess
import sys
import tempfile
import threading
import time
import logging

ROOT = Path(__file__).resolve().parents[2]
_DEPS = str(ROOT / "build/mcp-test-deps")
site.addsitedir(_DEPS)
# site.addsitedir 把目录追加到 sys.path 末尾，系统自带的同名包会被抢先导入
# （本机 jsonschema 来自 /usr/lib/python3/dist-packages），导致 SDK 的 schema
# 校验器签名不匹配。提到最前，让 tests/mcp/requirements.txt 锁定的版本生效。
if _DEPS in sys.path:
    sys.path.remove(_DEPS)
sys.path.insert(0, _DEPS)
import paramiko
logging.getLogger("paramiko").setLevel(logging.CRITICAL)


class SftpRoot(paramiko.SFTPServerInterface):
    def __init__(self, server, root: str, *args, **kwargs):
        super().__init__(server, *args, **kwargs)
        self.root = Path(root).resolve()

    def _local(self, path: str) -> Path:
        normalized = posixpath.normpath("/" + path.lstrip("/"))
        if normalized == "/.." or normalized.startswith("/../"):
            raise OSError("path escapes loopback SFTP root")
        return (self.root / normalized.lstrip("/")).resolve()

    def canonicalize(self, path):
        return posixpath.normpath("/" + path.lstrip("/"))

    def stat(self, path):
        try:
            return paramiko.SFTPAttributes.from_stat(os.stat(self._local(path)))
        except FileNotFoundError:
            return paramiko.SFTP_NO_SUCH_FILE
        except OSError:
            return paramiko.SFTP_FAILURE

    def lstat(self, path):
        try:
            return paramiko.SFTPAttributes.from_stat(os.lstat(self._local(path)))
        except FileNotFoundError:
            return paramiko.SFTP_NO_SUCH_FILE
        except OSError:
            return paramiko.SFTP_FAILURE

    def open(self, path, flags, attr):
        try:
            local = self._local(path)
            descriptor = os.open(local, flags, 0o600)
            if flags & os.O_WRONLY:
                stream = os.fdopen(descriptor, "wb")
            elif flags & os.O_RDWR:
                stream = os.fdopen(descriptor, "r+b")
            else:
                stream = os.fdopen(descriptor, "rb")
            handle = paramiko.SFTPHandle(flags)
            handle.filename = path
            if flags & os.O_WRONLY:
                handle.writefile = stream
            else:
                handle.readfile = stream
            return handle
        except FileNotFoundError:
            return paramiko.SFTP_NO_SUCH_FILE
        except OSError:
            return paramiko.SFTP_PERMISSION_DENIED

    def chattr(self, path, attr):
        try:
            if attr.st_mode is not None:
                os.chmod(self._local(path), attr.st_mode)
            return paramiko.SFTP_OK
        except OSError:
            return paramiko.SFTP_FAILURE


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
        def shell():
            pending = bytearray()
            try:
                channel.sendall(b"root# ")
                while True:
                    received = channel.recv(4096)
                    if not received:
                        return
                    pending.extend(received)
                    while b"\r" in pending:
                        line, _, remainder = pending.partition(b"\r")
                        pending = bytearray(remainder)
                        marker = re.search(rb"NT;END;([0-9a-f-]+);%d", line)
                        if not line.startswith(b"pwd;__nvterm_rc=$?;printf") or not marker:
                            continue
                        # 只模拟终端回显与结束证据，不解释或执行请求正文。
                        channel.sendall(b"root# " + line + b"\r\n/fixture\r\n")
                        channel.sendall(b"\x1b]633;NT;END;" + marker.group(1)
                                        + b";0\x07root# ")
            except (OSError, EOFError):
                return
        threading.Thread(target=shell, daemon=True).start()
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
    fixture_root = tempfile.TemporaryDirectory(prefix="novaterm-sftp-loopback-")
    sftp_root = Path(fixture_root.name)
    (sftp_root / "scripts").mkdir()
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(10)
    transports = []
    stop = threading.Event()
    host_key = paramiko.RSAKey.generate(2048)

    def serve_client(client):
        try:
            transport = paramiko.Transport(client)
            transports.append(transport)
            transport.add_server_key(host_key)
            transport.set_subsystem_handler("sftp", paramiko.SFTPServer,
                                            SftpRoot, root=str(sftp_root))
            transport.start_server(server=Server())
            channels = []
            while transport.is_active() and not stop.is_set():
                channel = transport.accept(0.1)
                if channel is not None:
                    channels.append(channel)
        except (OSError, EOFError):
            pass

    def serve():
        while not stop.is_set():
            try:
                client, _ = listener.accept()
                threading.Thread(target=serve_client, args=(client,), daemon=True).start()
            except socket.timeout:
                continue
            except OSError:
                return

    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    try:
        try:
            result = subprocess.run(
                [str(executable), "--ssh-loopback-check", str(listener.getsockname()[1])],
                timeout=30, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except subprocess.TimeoutExpired as timeout:
            if timeout.stdout:
                print(timeout.stdout.decode("utf-8", errors="replace"), end="", file=sys.stderr)
            if timeout.stderr:
                print(timeout.stderr.decode("utf-8", errors="replace"), end="", file=sys.stderr)
            raise
        print(result.stdout.decode("utf-8", errors="replace"), end="")
        if result.returncode:
            print(result.stderr.decode("utf-8", errors="replace"), file=sys.stderr, end="")
            print(f"SSH fixture process exit: {result.returncode:#x}", file=sys.stderr)
        if result.returncode:
            raise SystemExit(result.returncode)
        script = sftp_root / "scripts" / "novaterm-loopback.sh"
        expected = b"#!/bin/sh\nprintf loopback-only\n"
        if not script.exists() or script.read_bytes() != expected:
            raise SystemExit("SFTP loopback script bytes did not match the accepted content")
        print("loopback SFTP script upload: PASS")
    finally:
        stop.set()
        listener.close()
        for transport in transports:
            transport.close()
        worker.join(timeout=3)
        fixture_root.cleanup()


if __name__ == "__main__":
    main()
