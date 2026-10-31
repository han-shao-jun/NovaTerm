"""Loopback SFTP server exercises local-source rejection before remote creation."""
import socket
import stat
import struct
import subprocess
import threading
import time
from pathlib import Path
import site
import sys

ROOT = Path(__file__).resolve().parents[2]
site.addsitedir(str(ROOT / "build/mcp-test-deps"))
import paramiko
from paramiko.sftp import CMD_INIT, CMD_VERSION, CMD_ATTRS, CMD_EXTENDED, _VERSION
from paramiko.message import Message

class Server(paramiko.ServerInterface):
    def check_auth_password(self, username, password):
        return paramiko.AUTH_SUCCESSFUL if (username, password) == ("fixture", "fixture-only") else paramiko.AUTH_FAILED
    def get_allowed_auths(self, username): return "password"
    def check_channel_request(self, kind, chanid):
        return paramiko.OPEN_SUCCEEDED if kind == "session" else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

class Root(paramiko.SFTPServerInterface):
    opens = 0
    reads = 0
    writes = 0
    publications = 0
    file_size = 0
    stall = False
    partial = False
    timeout_test = False
    download = False
    write_mode = False
    publication = False
    total_deadline = False
    def canonicalize(self, path): return "/" if path == "/" else path
    def lstat(self, path):
        if self.stall: time.sleep(4)
        if self.download and path == "/remote.bin": return self.file_attributes(size=1048576 if self.total_deadline else 4)
        if (self.write_mode or self.publication) and path.startswith("/.novaterm-upload-"):
            return self.file_attributes(size=Root.file_size)
        if path != "/": return paramiko.SFTP_NO_SUCH_FILE
        attr = paramiko.SFTPAttributes()
        attr.st_mode = stat.S_IFDIR | 0o700
        attr.st_uid = attr.st_gid = 1000
        return attr
    @staticmethod
    def file_attributes(size=4):
        attr = paramiko.SFTPAttributes()
        attr.st_mode = stat.S_IFREG | 0o600
        attr.st_uid = attr.st_gid = 1000
        attr.st_size = size; attr.st_atime = attr.st_mtime = 1
        return attr
    def open(self, path, flags, attr):
        Root.opens += 1
        if self.download and path == "/remote.bin":
            class Handle(paramiko.SFTPHandle):
                def stat(self): return Root.file_attributes(size=1048576 if Root.total_deadline else 4)
                def read(self, offset, length):
                    Root.reads += 1
                    time.sleep(13 if Root.total_deadline else 4)
                    return b"x" * min(length, 1048576 - offset) if Root.total_deadline else b"test"[offset:offset + length]
            return Handle(flags)
        if (self.write_mode or self.publication) and path.startswith("/.novaterm-upload-"):
            class Handle(paramiko.SFTPHandle):
                def stat(self): return Root.file_attributes(size=Root.file_size)
                def write(self, offset, data):
                    Root.writes += 1
                    if Root.write_mode: time.sleep(4)
                    Root.file_size = max(Root.file_size, offset + len(data))
                    return paramiko.SFTP_OK
            return Handle(flags)
        return paramiko.SFTP_PERMISSION_DENIED

class Sftp(paramiko.SFTPServer):
    def _send_server_version(self):
        kind, data = self._read_packet()
        if kind != CMD_INIT: raise RuntimeError("expected INIT")
        version = struct.unpack(">I", data[:4])[0]
        message = Message(); message.add_int(_VERSION)
        message.add("hardlink@openssh.com", "1")
        self._send_packet(CMD_VERSION, message)
        return version
    def _send_packet(self, kind, packet):
        if Root.partial and kind == CMD_ATTRS:
            data = packet.asbytes()
            wire = struct.pack(">I", len(data) + 1) + bytes([kind]) + data
            for byte in wire:
                self._write_all(bytes([byte]))
                time.sleep(1 if Root.timeout_test else 0.15)
            return
        super()._send_packet(kind, packet)
    def _process(self, kind, request_number, message):
        if Root.publication and kind == CMD_EXTENDED:
            tag = message.get_text()
            if tag == "hardlink@openssh.com":
                message.get_text(); message.get_text()
                Root.publications += 1
                time.sleep(20)
                self._send_status(request_number, paramiko.SFTP_OK)
                return
        super()._process(kind, request_number, message)

def main():
    timeout_test = "--timeout" in sys.argv[1:]
    Root.publication = "--publication" in sys.argv[1:]
    timeout_test = timeout_test or Root.publication
    Root.total_deadline = "--total" in sys.argv[1:]
    Root.download = "--download" in sys.argv[1:] or Root.total_deadline
    Root.write_mode = "--write" in sys.argv[1:]
    Root.timeout_test = timeout_test
    Root.partial = "--partial" in sys.argv[1:] or (timeout_test and not Root.publication)
    Root.stall = "--cancel" in sys.argv[1:] and not Root.partial
    listener = socket.socket(); listener.bind(("127.0.0.1", 0)); listener.listen(4)
    listener.settimeout(0.2)
    key = paramiko.RSAKey.generate(2048)
    errors = []
    stopping = threading.Event()
    def handle(client):
        try:
            transport = paramiko.Transport(client)
            transport.add_server_key(key)
            transport.set_subsystem_handler("sftp", Sftp, Root)
            transport.start_server(server=Server())
            channels = []
            while transport.is_active():
                channel = transport.accept(0.2)
                if channel is not None: channels.append(channel)
            transport.close()
        except Exception as exc:
            errors.append(repr(exc))
    def run():
        while not stopping.is_set():
            try:
                client, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=handle, args=(client,), daemon=True).start()
    worker = threading.Thread(target=run, daemon=True); worker.start()
    independent = None
    executable = Path(next((arg for arg in sys.argv[1:] if not arg.startswith("--")),
                           ROOT / "build/bin/novaterm_sftp_session_safety_tests.exe"))
    try:
        if Root.partial and not timeout_test:
            independent = paramiko.Transport(("127.0.0.1", listener.getsockname()[1]))
            independent.connect(username="fixture", password="fixture-only")
            independent_sftp = paramiko.SFTPClient.from_transport(independent)
            assert independent_sftp.normalize("/") == "/"
        mode = "--total-download-loopback" if Root.total_deadline else "--publication-timeout-loopback" if Root.publication else "--cancel-download-sftp-loopback" if Root.download else "--timeout-sftp-loopback" if timeout_test else "--cancel-sftp-loopback" if Root.stall or Root.partial or Root.write_mode else "--oversize-sftp-loopback"
        result = subprocess.run([str(executable), mode, str(listener.getsockname()[1])],
                                capture_output=True, text=True, timeout=130 if Root.total_deadline else 25)
        print(result.stdout, end=""); print(result.stderr, end="", file=sys.stderr)
        if result.returncode or (Root.download and (Root.opens != 1 or Root.reads < (2 if Root.total_deadline else 1))) or (Root.write_mode and (Root.opens != 1 or Root.writes != 1)) or (Root.publication and Root.publications != 1) or (not Root.download and not Root.write_mode and not Root.publication and Root.opens):
            raise AssertionError(f"exit={result.returncode}, remote opens={Root.opens}, reads={Root.reads}, writes={Root.writes}, publications={Root.publications}")
        if independent is not None:
            assert independent.is_active() and independent_sftp.normalize("/") == "/"
            print("Independent SSH connection: PASS")
        print("SFTP total deadline: PASS" if Root.total_deadline else "SFTP timeout: PASS" if timeout_test else "SFTP cancellation: PASS" if Root.stall or Root.partial or Root.download or Root.write_mode else "SFTP oversize upload: PASS; no remote file opened")
    finally:
        if independent is not None: independent.close()
        stopping.set(); listener.close(); worker.join(timeout=3)
    if errors: raise RuntimeError(errors)

if __name__ == "__main__": main()
