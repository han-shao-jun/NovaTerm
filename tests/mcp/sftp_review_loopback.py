"""只在回环 SSH 注入异常目录/传输响应，验证生产 SftpSession 的安全边界。"""
from pathlib import Path
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "build/mcp-test-deps"))
import paramiko
from paramiko.sftp import CMD_READDIR, CMD_CLOSE
from sftp_init_failure_loopback import Server


class Handle(paramiko.SFTPHandle):
    def __init__(self, scenario):
        super().__init__()
        self.scenario = scenario

    def write(self, offset, data):
        return paramiko.SFTP_PERMISSION_DENIED if "write" in self.scenario else paramiko.SFTP_OK

    def close(self):
        return paramiko.SFTP_PERMISSION_DENIED if "close" in self.scenario else paramiko.SFTP_OK


class Files(paramiko.SFTPServerInterface):
    def __init__(self, server, scenario, root):
        super().__init__(server)
        self.scenario, self.root = scenario, root

    def canonicalize(self, path):
        return "/" if path in (".", "/") else path

    def stat(self, path):
        result = paramiko.SFTPAttributes()
        result.st_mode = stat.S_IFDIR | 0o755
        result.st_size = 0
        return result

    def open(self, path, flags, attr):
        return Handle(self.scenario)

    def remove(self, path):
        return paramiko.SFTP_FAILURE

    def rename(self, old, new):
        return paramiko.SFTP_PERMISSION_DENIED

    def list_folder(self, path):
        if self.scenario == "tree-depth-boundary":
            names = ["child"] if len(path.strip("/").split("/")) < 64 else [".", ".."]
        elif self.scenario.endswith("unsafe"):
            names = ["../outside/file"]
        elif self.scenario.endswith("depth"):
            if path.count("/") > 80:
                return paramiko.SFTP_PERMISSION_DENIED
            names = ["child"]
        else:
            names = [f"file-{i}" for i in range(20000)]
        entries = []
        for name in names:
            item = paramiko.SFTPAttributes()
            item.filename, item.st_size = name, 0
            item.st_mode = (stat.S_IFDIR if self.scenario.endswith("depth") or self.scenario == "tree-depth-boundary" else stat.S_IFREG) | 0o644
            entries.append(item)
        return entries


class Protocol(paramiko.SFTPServer):
    def _send_status(self, request, code, desc=None):
        if code == paramiko.SFTP_PERMISSION_DENIED:
            desc = "PRIMARY_DENIED"
        elif code == paramiko.SFTP_FAILURE:
            desc = "CLEANUP_FAILED"
        return super()._send_status(request, code, desc)

    def _process(self, kind, request, message):
        if kind == CMD_CLOSE and self.server.scenario.endswith("close"):
            handle = message.get_binary()
            if handle in self.file_table:
                del self.file_table[handle]
                self._send_status(request, paramiko.SFTP_PERMISSION_DENIED)
                return
        if kind == CMD_READDIR:
            (self.server.root / "started").touch()
            if self.server.scenario == "list-deadline":
                if not hasattr(self, "enumeration_started"):
                    self.enumeration_started = time.monotonic()
                if time.monotonic() - self.enumeration_started >= 29:
                    time.sleep(2)
                    self._send_status(request, paramiko.SFTP_EOF)
                    return
                time.sleep(0.25)
            if self.server.scenario.endswith("cancel"):
                time.sleep(0.02)
        return super()._process(kind, request, message)


def run(executable, scenario):
    with tempfile.TemporaryDirectory(prefix="sftp-review-") as temp:
        root = Path(temp)
        (root / "source").mkdir()
        (root / "source/file.bin").write_bytes(b"fixture")
        errors = []
        key = paramiko.RSAKey.generate(2048)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(10)
            port = listener.getsockname()[1]
            hosts = root / "known_hosts"
            hosts.write_text(f"[127.0.0.1]:{port} {key.get_name()} {key.get_base64()}\n")

            def serve():
                try:
                    client, _ = listener.accept()
                    with paramiko.Transport(client) as transport:
                        transport.add_server_key(key)
                        transport.set_subsystem_handler("sftp", Protocol, Files, scenario, root)
                        transport.start_server(server=Server())
                        channel = transport.accept(10)
                        deadline = time.monotonic() + (4 if scenario.endswith("cancel") else 50 if scenario.endswith("deadline") else 18)
                        while transport.is_active() and channel and not channel.closed and time.monotonic() < deadline:
                            time.sleep(0.01)
                except Exception as error:
                    errors.append(repr(error))

            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            result = subprocess.run([str(executable), "--review-loopback", scenario, str(port), str(hosts), str(root)],
                                    capture_output=True, text=True, timeout=55 if scenario.endswith("deadline") else 25)
            thread.join(timeout=5)
            print(result.stdout, end="", flush=True)
            print(result.stderr, end="", file=sys.stderr)
            ok = result.returncode == 0 and not errors and not thread.is_alive()
            print(f"{scenario}: {'PASS' if ok else 'FAIL'} exit={result.returncode} server_errors={errors}", flush=True)
            return ok


def main():
    executable = Path(sys.argv[1]).resolve()
    scenarios = sys.argv[2:] or ["list-unsafe", "list-limit", "tree-limit", "tree-depth",
        "list-cancel", "tree-cancel", "directory-write", "directory-close", "directory-rename",
        "script-write", "file-rename", "tree-depth-boundary", "list-deadline"]
    results = [run(executable, scenario) for scenario in scenarios]
    print(f"passed={sum(results)} total={len(results)}")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
