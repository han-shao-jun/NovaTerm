#!/usr/bin/env python3
"""通过本机 lrzsz、socat 虚拟串口和真实 Session 执行文件大小矩阵。"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys

MODES = {"x-checksum": "0", "x-crc": "1", "x-1k": "2", "y": "3", "z": "4"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--protocol", choices=MODES)
    parser.add_argument("--direct-peer-pty", action="store_true",
                        help="直接把 lrzsz 挂到 PTY，复现其 TCIOFLUSH 收尾竞争")
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--output-dir", type=Path, default=Path("build/serial-lrzsz-results"))
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("当前虚拟串口验收仅支持 Linux")
    if args.jobs < 1 or not args.test_binary.is_file():
        parser.error("需要已构建的测试程序，jobs 必须为正整数")
    for tool in ("socat", "rz", "sz"):
        if not shutil.which(tool):
            parser.error(f"需要安装 {tool}")
    environment = dict(os.environ, NOVATERM_SERIAL_LRZSZ_TESTS="1", QT_QPA_PLATFORM="offscreen")
    environment["NOVATERM_SERIAL_LRZSZ_DIRECT_PTY"] = "1" if args.direct_peer_pty else "0"
    binary = str(args.test_binary.resolve())
    discovered = subprocess.run([binary, "-datatags"], env=environment, text=True,
                                capture_output=True, check=True, timeout=15).stdout
    modes = {args.protocol: MODES[args.protocol]} if args.protocol else MODES
    groups = {}
    for mode, prefix in modes.items():
        rows = [line.split()[2] for line in discovered.splitlines()
                if len(line.split()) == 3 and line.split()[1] == "serialLrzszSizeMatrix"
                and line.split()[2].startswith(prefix + "-")]
        if not rows:
            parser.error(f"测试程序没有 {mode} 数据行，请重建 novaterm_session_tests")
        groups[mode] = rows
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    print(subprocess.run(["rz", "--version"], capture_output=True, text=True,
                         check=True, timeout=5).stdout.strip(), flush=True)

    def run(mode, rows):
        log = output / f"{mode}.txt"
        command = [binary] + ["serialLrzszSizeMatrix:" + row for row in rows]
        command += ["-o", str(log) + ",txt"]
        print(f"START {mode} cases={len(rows)} log={log}", flush=True)
        # 各协议只有自己的临时串口/文件；超时回收整个进程组，包括对端。
        process = subprocess.Popen(command, env=environment, start_new_session=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        def reap_group():
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        try:
            _, error = process.communicate(timeout=len(rows) * 40 + 15)
        except subprocess.TimeoutExpired:
            reap_group()
            process.communicate()
            print(f"FAIL {mode}: matrix timeout, log={log}", flush=True)
            return False
        finally:
            # Qt 崩溃时 C++ 析构不会执行，成功/失败均兜底回收残留对端。
            reap_group()
        report = log.read_text() if log.exists() else ""
        passed = sum(f"PASS   : SessionTests::serialLrzszSizeMatrix({row})" in report for row in rows)
        ok = process.returncode == 0 and passed == len(rows)
        print(f"{'PASS' if ok else 'FAIL'} {mode} cases={passed}/{len(rows)} log={log}", flush=True)
        if not ok:
            print(error[-2000:], flush=True)
            print("\n".join(line[:300] for line in report.splitlines()
                            if "FAIL!" in line or "Totals:" in line), flush=True)
        return ok

    with ThreadPoolExecutor(max_workers=min(args.jobs, len(groups))) as pool:
        results = [future.result() for future in as_completed(
            [pool.submit(run, mode, rows) for mode, rows in groups.items()])]
    print(f"{'PASS' if all(results) else 'FAIL'} virtual serial total={sum(map(len, groups.values()))}",
          flush=True)
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
