"""Loopback-only SFTP fixture for serializing a slow recursive delete."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "build/mcp-test-deps"))
import paramiko


class Server(paramiko.ServerInterface):
    def check_auth_password(self, username, password):
        return paramiko.AUTH_SUCCESSFUL if (username, password) == ("fixture", "fixture-only") else paramiko.AUTH_FAILED

    def check_channel_request(self, kind, channel_id):
        return paramiko.OPEN_SUCCEEDED if kind == "session" else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED


class Files(paramiko.SFTPServerInterface):
    root = None

    def local(self, path):
        target = (self.root / path.lstrip("/")).resolve()
        if not target.is_relative_to(self.root):
            raise ValueError("path outside fixture")
        return target

    def canonicalize(self, path):
        return "/" if path in (".", "/") else path

    def list_folder(self, path):
        try:
            result = []
            for entry in self.local(path).iterdir():
                attributes = paramiko.SFTPAttributes.from_stat(entry.lstat())
                attributes.filename = entry.name
                result.append(attributes)
            return result
        except OSError:
            return paramiko.SFTP_FAILURE

    def remove(self, path):
        try:
            time.sleep(0.003)
            self.local(path).unlink()
            return paramiko.SFTP_OK
        except OSError:
            return paramiko.SFTP_FAILURE

    def rmdir(self, path):
        try:
            self.local(path).rmdir()
            return paramiko.SFTP_OK
        except OSError:
            return paramiko.SFTP_FAILURE


def main():
    with tempfile.TemporaryDirectory(prefix="novaterm-sftp-delete-") as temp:
        Files.root = Path(temp).resolve()
        folder = Files.root / "large"
        (folder / "nested").mkdir(parents=True)
        for index in range(600):
            (folder / ("nested" if index % 2 else "") / f"{index}.txt").write_text("fixture")
        (Files.root / "keep.txt").write_text("sentinel")
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(4)
        listener.settimeout(0.2)
        port = listener.getsockname()[1]
        key = paramiko.RSAKey.generate(2048)
        known_hosts = Files.root / "known_hosts"
        known_hosts.write_text(f"[127.0.0.1]:{port} {key.get_name()} {key.get_base64()}\n")
        stop = threading.Event()
        errors = []

        def handle(client):
            try:
                transport = paramiko.Transport(client)
                transport.add_server_key(key)
                transport.set_subsystem_handler("sftp", paramiko.SFTPServer, Files)
                transport.start_server(server=Server())
                channels = []
                while transport.is_active() and not stop.is_set():
                    channel = transport.accept(0.1)
                    if channel is not None:
                        channels.append(channel)
                transport.close()
            except Exception as exc:
                errors.append(repr(exc))

        def accept():
            while not stop.is_set():
                try:
                    client, _ = listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    return
                threading.Thread(target=handle, args=(client,), daemon=True).start()

        thread = threading.Thread(target=accept, daemon=True)
        thread.start()
        executable = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/Release/bin/novaterm_sftp_session_safety_tests.exe"
        try:
            result = subprocess.run([str(executable), "--delete-tree-loopback", str(port), str(known_hosts)],
                                    capture_output=True, text=True, timeout=30)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode or folder.exists() or (Files.root / "keep.txt").read_text() != "sentinel":
                raise RuntimeError(f"exit={result.returncode}, folder_exists={folder.exists()}")
            print("600-file recursive delete, queued browsing and sentinel: PASS")
            (folder / "nested").mkdir(parents=True)
            for index in range(600):
                (folder / ("nested" if index % 2 else "") / f"{index}.txt").write_text("fixture")
            result = subprocess.run([str(executable), "--delete-tree-cancel-loopback", str(port), str(known_hosts)],
                                    capture_output=True, text=True, timeout=30)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            remaining = sum(1 for path in folder.rglob("*.txt") if path.is_file())
            if result.returncode or remaining == 0 or remaining == 600:
                raise RuntimeError(f"cancel exit={result.returncode}, remaining={remaining}")
            print(f"Stop preserves remaining entries: PASS ({remaining} files remain)")
        finally:
            stop.set()
            listener.close()
            thread.join(timeout=3)
        if errors:
            raise RuntimeError(errors)


if __name__ == "__main__":
    main()
