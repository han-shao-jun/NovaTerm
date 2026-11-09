"""用真实 OpenSSH 校验未协商 limits 时的大文件上传及后续目录刷新。"""
from pathlib import Path
import hashlib
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "build/mcp-test-deps"))
import paramiko
from sftp_init_failure_loopback import Server


def read_exact(read, size):
    result = b""
    while len(result) < size:
        chunk = read(size - len(result))
        if not chunk:
            raise EOFError("SFTP stream closed")
        result += chunk
    return result


class OpenSSH(paramiko.SubsystemHandler):
    def start_subsystem(self, name, transport, channel):
        self.server = self.get_server()
        server = subprocess.Popen(
            ["/usr/lib/openssh/sftp-server", "-e", "-P", "limits"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            bufsize=0)
        errors = []

        def forward_requests():
            try:
                while True:
                    header = read_exact(channel.recv, 4)
                    size = struct.unpack(">I", header)[0]
                    packet = read_exact(channel.recv, size)
                    if packet[0] == 6:
                        self.server.write_packets.append(size)
                    # 无缓冲管道的 write 允许短写，必须写完再读取下一个包。
                    data = memoryview(header + packet)
                    while data:
                        data = data[server.stdin.write(data):]
            except (EOFError, BrokenPipeError, OSError):
                pass
            finally:
                server.stdin.close()

        sender = threading.Thread(target=forward_requests, daemon=True)
        sender.start()
        try:
            header = read_exact(server.stdout.read, 4)
            version = read_exact(server.stdout.read, struct.unpack(">I", header)[0])
            if version[0] != 2 or b"limits@openssh.com" in version:
                raise RuntimeError("fixture must omit limits@openssh.com")
            channel.sendall(header + version)
            while True:
                data = server.stdout.read(65536)
                if not data:
                    break
                channel.sendall(data)
        except (EOFError, OSError) as error:
            errors.append(str(error))
        finally:
            channel.close()
            sender.join(timeout=5)
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
            self.server.diagnostics.append(server.stderr.read().decode())
            self.server.errors.extend(errors)
            self.server.done.set()


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="novaterm-sftp-packet-") as temp:
        root = Path(temp)
        source, destination = root / "source.bin", root / "uploaded.bin"
        source.write_bytes(os.urandom(1024 * 1024 + 137))
        key = paramiko.RSAKey.generate(2048)
        server = Server()
        server.write_packets, server.diagnostics, server.errors = [], [], []
        server.done = threading.Event()
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(10)
            port = listener.getsockname()[1]
            known_hosts = root / "known_hosts"
            known_hosts.write_text(f"[127.0.0.1]:{port} {key.get_name()} {key.get_base64()}\n")

            def serve():
                try:
                    client, _ = listener.accept()
                    with paramiko.Transport(client) as transport:
                        transport.add_server_key(key)
                        transport.set_subsystem_handler("sftp", OpenSSH)
                        transport.start_server(server=server)
                        channel = transport.accept(10)
                        if channel is None or not server.done.wait(25):
                            raise RuntimeError("SFTP fixture timed out")
                except Exception as error:
                    server.errors.append(repr(error))

            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            result = subprocess.run(
                [str(executable), "--upload-packet-loopback", str(port),
                 str(known_hosts), str(source), str(destination)],
                capture_output=True, text=True, timeout=30)
            thread.join(timeout=10)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            print("write_packet_sizes=", server.write_packets)
            print("server_stderr=", server.diagnostics)
            leftovers = [(p.name, p.stat().st_size) for p in root.glob("*.novaterm-upload-*")]
            equal = destination.exists() and destination.read_bytes() == source.read_bytes()
            print(f"exit={result.returncode} identical={equal} leftovers={leftovers}")
            if (result.returncode or not equal or leftovers or server.errors
                    or thread.is_alive() or not server.write_packets
                    or max(server.write_packets) > 256 * 1024):
                raise RuntimeError(f"upload regression failed: {server.errors}")
            print("sha256=", hashlib.sha256(destination.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
