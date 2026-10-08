#!/usr/bin/env python3
"""三个协议与固定 lrzsz 对端的 raw PTY 互通，全部文件位于临时目录。"""
import argparse
import errno
import hashlib
import os
from pathlib import Path
import pty
import selectors
import socket
import subprocess
import tempfile
import time
import tty


def run_pair(driver, lrzsz_dir, protocol, send, files, destination, trace=False):
    """raw PTY 与独立对端透明字节桥接；不连接用户串口或服务器。"""
    xy = protocol.startswith("x-")
    option = ["--xmodem"] if xy else ["--ymodem"] if protocol == "y" else []
    command = [str(driver), protocol, "send" if send else "receive"]
    command += [str(path) for path in files] if send else [str(destination)]
    # XMODEM 不带长度：显式提供大小以便验证原文件尾部不会被猜测裁剪。
    if not send and xy:
        command.append(str(files[0].stat().st_size))
    peer = [str(lrzsz_dir / ("lrz" if send else "lsz")), "-b"] + option
    if send and xy:
        peer += [str(destination / "received.bin")]
        if protocol != "x-checksum":
            peer += ["-c"]
    if not send:
        if protocol == "x-1k":
            peer += ["-k"]
        if protocol == "z16":
            peer += ["-o"]
        peer += [str(path) for path in files]
    resources = []
    processes = []
    selector = selectors.DefaultSelector()
    logs = []
    try:
        for index, argv in enumerate((command, peer)):
            if index == 0:
                master, slave = pty.openpty()
                tty.setraw(slave)
            else:
                # lrzsz 在 io_mode(0,0) 中 tcflush(TCIOFLUSH)，会删除 PTY
                # 尚未由 master 消费的最后 ACK。对端走透明 socketpair，
                # driver 仍经 raw PTY；不改上游源码，不伪造协议 ACK。
                left, right = socket.socketpair()
                master, slave = left.detach(), right.detach()
            resources += [master, slave]
            os.set_blocking(master, False)
            log = tempfile.TemporaryFile()
            logs.append(log)
            process = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=log,
                                       cwd=destination, close_fds=True)
            processes.append(process)
            os.close(slave)
            resources.remove(slave)
            selector.register(master, selectors.EVENT_READ, index)
        masters = [key.fd for key in selector.get_map().values()]
        pending = [bytearray(), bytearray()]
        total = [0, 0]
        deadline = time.monotonic() + 70
        while time.monotonic() < deadline:
            for target in (0, 1):
                if pending[target]:
                    try:
                        count = os.write(masters[target], pending[target][:8192])
                        del pending[target][:count]
                    except BlockingIOError:
                        pass
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
            if all(process.poll() is not None for process in processes):
                break
            for key, _ in selector.select(0.005):
                origin = key.data
                # 桥接缓存上限 256 KiB，慢对端不会导致测试自身无限分配。
                if len(pending[1 - origin]) > 256 * 1024 - 4096:
                    continue
                try:
                    data = os.read(key.fd, 4096)
                except BlockingIOError:
                    continue
                except OSError as error:
                    if error.errno == errno.EIO:
                        continue
                    raise
                if data:
                    if trace and total[origin] < 10000:
                        print(f"WIRE {origin}: {data[:256].hex()}", flush=True)
                    pending[1 - origin].extend(data)
                    total[origin] += len(data)
        else:
            raise RuntimeError(f"deadline: {protocol}, send={send}, wire={total}")
        for process in processes:
            process.wait(timeout=3)
        if any(process.returncode != 0 for process in processes):
            raise RuntimeError(f"exit codes {[p.returncode for p in processes]}, wire={total}")
        if xy:
            target = destination / "received.bin"
            expected = files[0].read_bytes()
            actual = target.read_bytes()
            if send:
                # lrzsz 的 XMODEM 接收保留填充；不把来源内容与协议填充混为一谈。
                block = 1024 if protocol == "x-1k" else 128
                padding = (-len(expected)) % block
                if actual[:len(expected)] != expected or len(actual) != len(expected) + padding:
                    raise RuntimeError("XMODEM payload/padding mismatch")
                if actual[len(expected):] != b"\x1a" * padding:
                    raise RuntimeError("XMODEM padding bytes mismatch")
            elif actual != expected:
                raise RuntimeError("XMODEM exact-size content mismatch")
        else:
            for source in files:
                target = destination / source.name
                if hashlib.sha256(source.read_bytes()).digest() != hashlib.sha256(target.read_bytes()).digest():
                    raise RuntimeError(f"SHA256 mismatch: {source.name}")
        return total
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait()
        for index, log in enumerate(logs):
            log.seek(0)
            text = log.read().decode("utf-8", errors="replace")
            if any(p.returncode != 0 for p in processes):
                print(f"stderr[{index}]: {text[-4000:]}", flush=True)
            log.close()
        selector.close()
        for fd in resources:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--lrzsz-dir", type=Path, required=True)
    parser.add_argument("--protocol", choices=["x-checksum", "x-crc", "x-1k", "y", "z", "z16"])
    parser.add_argument("--trace", action="store_true")
    parser.add_argument("--skip-known-lrzsz-bugs", action="store_true",
                        help="显式跳过 lrzsz 0.12.20 CRC16 空文件发送崩溃用例")
    parser.add_argument("--large", action="store_true", help="另外传输 64 KiB、1/5/10 MiB 四档文件")
    args = parser.parse_args()
    if not args.driver.is_file() or not all((args.lrzsz_dir / name).is_file() for name in ("lrz", "lsz")):
        parser.error("未运行：缺少 driver 或独立 lrzsz 对端")
    peer_version = subprocess.run([str(args.lrzsz_dir.resolve() / "lsz"), "--version"],
                                  capture_output=True, text=True, check=True).stdout
    skip_legacy_empty = args.skip_known_lrzsz_bugs and "0.12.20" in peer_version
    modes = [args.protocol] if args.protocol else ["x-checksum", "x-crc", "x-1k", "y", "z", "z16"]
    cases = [0, 1, 127, 128, 129, 1023, 1024, 1025, 8191, 8192, 8193, 33793]
    if args.large:
        cases += [64 * 1024, 1024 * 1024, 5 * 1024 * 1024, 10 * 1024 * 1024]
    count = 0
    skipped = 0
    with tempfile.TemporaryDirectory(prefix="novaterm-protocol-interop-") as directory:
        root = Path(directory)
        for mode in modes:
            # X/Y 的一字节序号在 255→0 回绕，覆盖回绕前后及下一包。
            mode_cases = set(cases)
            if mode.startswith("x-") or mode == "y":
                block = 1024 if mode in ("x-1k", "y") else 128
                mode_cases.update(packets * block + delta
                                  for packets in (255, 256, 257) for delta in (-1, 0, 1))
            for size in sorted(mode_cases):
                source = root / f"source-{mode}-{size}.bin"
                data = (bytes(range(256)) * ((size + 255) // 256))[:size]
                if size >= 2:
                    data = data[:-2] + b"\x1a\x00"
                source.write_bytes(data)
                for send in (True, False):
                    if skip_legacy_empty and mode == "z16" and not send and size == 0:
                        print("SKIP z16 receive size=0: lrzsz 0.12.20 zsdata zero-length bug", flush=True)
                        skipped += 1
                        continue
                    destination = root / f"out-{count}"
                    destination.mkdir()
                    started = time.monotonic()
                    wire = run_pair(args.driver.resolve(), args.lrzsz_dir.resolve(), mode,
                                    send, [source], destination, args.trace)
                    print(f"PASS {mode} {'send' if send else 'receive'} size={size} wire={wire} elapsed={time.monotonic()-started:.3f}s", flush=True)
                    count += 1
            if mode in ("y", "z", "z16"):
                files = [root / "batch-empty.bin", root / "batch-中文.bin", root / "batch-tail.bin"]
                for index, source in enumerate(files):
                    source.write_bytes(bytes(range(256)) * index + b"\x1a\x00" * index)
                for send in (True, False):
                    if skip_legacy_empty and mode == "z16" and not send:
                        print("SKIP z16 receive empty-containing batch: lrzsz 0.12.20 zsdata zero-length bug", flush=True)
                        skipped += 1
                        continue
                    destination = root / f"out-{count}"
                    destination.mkdir()
                    started = time.monotonic()
                    wire = run_pair(args.driver.resolve(), args.lrzsz_dir.resolve(), mode,
                                    send, files, destination, args.trace)
                    print(f"PASS {mode} {'send' if send else 'receive'} batch=3 wire={wire} elapsed={time.monotonic()-started:.3f}s", flush=True)
                    count += 1
    print(f"PASS total={count} skipped={skipped} (independent lrzsz, raw PTY/socketpair, SHA256/padding verified)")


if __name__ == "__main__":
    main()
