"""Exercise SFTP initialization failure after a real SSH subsystem opens."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "build/mcp-test-deps"))
import paramiko


class Server(paramiko.ServerInterface):
    def check_auth_password(self, username, password):
        return (paramiko.AUTH_SUCCESSFUL if (username, password) == ("fixture", "fixture-only")
                else paramiko.AUTH_FAILED)

    def check_channel_request(self, kind, channel_id):
        return (paramiko.OPEN_SUCCEEDED if kind == "session"
                else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED)


class BadVersion(paramiko.SubsystemHandler):
    def start_subsystem(self, name, transport, channel):
        packet = b""
        while len(packet) < 9:
            chunk = channel.recv(9 - len(packet))
            if not chunk:
                raise RuntimeError("incomplete SSH_FXP_INIT")
            packet += chunk
        if packet[4] != 1:
            raise RuntimeError("expected SSH_FXP_INIT")
        # A valid packet with the wrong type forces sftp_init() to fail.
        channel.sendall(b"\x00\x00\x00\x05\x65\x00\x00\x00\x03")
        while transport.is_active() and not channel.closed:
            if not channel.recv(1024):
                break


def main():
    executable = Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="novaterm-sftp-init-") as temp:
        key = paramiko.RSAKey.generate(2048)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(10)
            port = listener.getsockname()[1]
            known_hosts = Path(temp) / "known_hosts"
            known_hosts.write_text(f"[127.0.0.1]:{port} {key.get_name()} {key.get_base64()}\n")
            result = []

            def serve():
                try:
                    client, _ = listener.accept()
                    transport = paramiko.Transport(client)
                    transport.add_server_key(key)
                    transport.set_subsystem_handler("sftp", BadVersion)
                    transport.start_server(server=Server())
                    channel = transport.accept(10)
                    if channel is None:
                        raise RuntimeError("no SSH channel")
                    while transport.is_active() and not channel.closed:
                        threading.Event().wait(0.05)
                    transport.close()
                except Exception as exc:
                    result.append(repr(exc))

            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            process = subprocess.run([str(executable), "--failed-sftp-init-loopback", str(port), str(known_hosts)],
                                     text=True, capture_output=True, timeout=20)
            thread.join(timeout=5)
            print(process.stdout, end="")
            print(process.stderr, end="", file=sys.stderr)
            if process.returncode or result or thread.is_alive():
                raise RuntimeError(f"exit={process.returncode}, server_errors={result}, server_alive={thread.is_alive()}")


if __name__ == "__main__":
    main()
