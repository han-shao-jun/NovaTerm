"""只监听回环的 SSH/SFTP 目录浏览夹具，不执行 shell 命令。"""
from __future__ import annotations
import os
from pathlib import Path
import posixpath
import site
import socket
import subprocess
import sys
import tempfile
import threading
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
        resolved = (self.root / normalized.lstrip("/")).resolve()
        if not resolved.is_relative_to(self.root):
            raise OSError("path escapes loopback SFTP root")
        return resolved

    def canonicalize(self, path):
        try:
            relative = self._local(path).relative_to(self.root).as_posix()
            return "/" if relative == "." else "/" + relative
        except OSError:
            return paramiko.SFTP_PERMISSION_DENIED

    def list_folder(self, path):
        try:
            result = []
            for entry in self._local(path).iterdir():
                attributes = paramiko.SFTPAttributes.from_stat(entry.lstat())
                attributes.filename = entry.name
                result.append(attributes)
            return result
        except OSError:
            return paramiko.SFTP_PERMISSION_DENIED

    def readlink(self, path):
        try:
            normalized = posixpath.normpath("/" + path.lstrip("/"))
            link = self.root / normalized.lstrip("/")
            if not link.parent.resolve().is_relative_to(self.root):
                return paramiko.SFTP_PERMISSION_DENIED
            return os.readlink(link)
        except OSError:
            return paramiko.SFTP_FAILURE

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


def main():
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/Release/bin/novaterm_mcp_tests.exe")
    fixture_root = tempfile.TemporaryDirectory(prefix="novaterm-sftp-loopback-")
    sftp_root = Path(fixture_root.name)
    for directory in ("browser/a", "browser/b", "browser/带 空格", "browser/big"):
        (sftp_root / directory).mkdir(parents=True, exist_ok=True)
    (sftp_root / "browser/ordinary-file").write_text("not a directory", encoding="utf-8")
    for index in range(1030):
        (sftp_root / f"browser/big/d{index:04}").mkdir()
    if os.name != "nt":
        (sftp_root / "browser/link-a").symlink_to("a", target_is_directory=True)
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
                [str(executable), "--directory-loopback-check", str(listener.getsockname()[1])],
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
        print("loopback SFTP directory browse: PASS")
    finally:
        stop.set()
        listener.close()
        for transport in transports:
            transport.close()
        worker.join(timeout=3)
        fixture_root.cleanup()


if __name__ == "__main__":
    main()
