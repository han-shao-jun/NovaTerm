#!/usr/bin/env python3
"""Capture WPR CPU samples during the serial input phase (Windows only).

Requires an administrator shell, WPR, and an already built serial_stress.exe.
Uses only the Python 3.9+ standard library. Existing WPR sessions are refused.
"""
import argparse
from datetime import datetime
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid


REPO_ROOT = Path(__file__).resolve().parents[3]


def positive_seconds(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("timeout must be a finite positive number")
    return number


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--executable-path", type=Path,
                        default=REPO_ROOT / "build/serial-stress-symbols/serial_stress.exe")
    result.add_argument("--output-directory", type=Path,
                        default=REPO_ROOT / "build/serial-stress-profile/captures")
    result.add_argument("--qt-prefix", type=Path,
                        default=Path(r"C:\Programs\Qt\6.8.3\msvc2022_64"))
    result.add_argument("--modes", nargs="+", choices=("visible", "hidden", "raw"),
                        default=["visible", "hidden"])
    result.add_argument("--port-a", default="COM1")
    result.add_argument("--port-b", default="COM2")
    result.add_argument("--setup-timeout", type=positive_seconds, default=120)
    result.add_argument("--input-timeout", type=positive_seconds, default=150)
    result.add_argument("--result-timeout", type=positive_seconds, default=60)
    return result


def require_idle_wpr(env):
    status = subprocess.run(["wpr", "-status"], env=env, cwd=str(REPO_ROOT),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace", timeout=15,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    # Refuse unknown/localized output as well: never infer that it is safe to stop.
    if status.returncode != 0 or "WPR is not recording" not in (status.stdout or ""):
        raise RuntimeError("Another WPR session is active, or WPR status is unknown; "
                           "it will not be interrupted. " + (status.stdout or "").strip())


def wait_marker(path, child, timeout, description):
    deadline = time.monotonic() + timeout
    while not path.exists():
        if child.poll() is not None:
            raise RuntimeError("Benchmark exited before " + description)
        if time.monotonic() >= deadline:
            raise RuntimeError(description + " timed out")
        time.sleep(0.1)


def profile_mode(args, mode, env):
    base = args.output_directory / (mode + "-" + datetime.now().strftime("%Y%m%d-%H%M%S")
                                    + "-" + uuid.uuid4().hex[:8])
    result_path = Path(str(base) + ".json")
    trace_path = Path(str(base) + ".etl")
    ready, go, done, release = (Path(str(base) + suffix)
                                for suffix in (".ready", ".go", ".done", ".release"))
    command = [str(args.executable_path), args.port_a, args.port_b, "8000000", "20",
               mode, "paced", str(result_path), "--bytes", "67108864",
               "--driver-rx-queue", "262144", "--stable-timers", "--deferred-verify",
               "--profile-handshake", str(base)]
    # Files cannot fill an unread PIPE; stdout and stderr stay available on failures.
    with Path(str(base) + ".stdout.txt").open("wb") as stdout, \
            Path(str(base) + ".stderr.txt").open("wb") as stderr:
        child = subprocess.Popen(command, cwd=str(REPO_ROOT), env=env,
                                 stdout=stdout, stderr=stderr,
                                 # Hide only the console, never pass SW_HIDE to Qt windows.
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        started = False
        failure = None
        cleanup_errors = []
        try:
            wait_marker(ready, child, args.setup_timeout, "Profiler handshake")
            require_idle_wpr(env)
            start = subprocess.run(["wpr", "-start", "CPU", "-filemode"], env=env,
                                   cwd=str(REPO_ROOT), stdout=stdout, stderr=stderr,
                                   timeout=30,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            if start.returncode:
                raise RuntimeError("WPR start failed: " + str(start.returncode))
            started = True
            go.write_text("go\n", encoding="ascii")
            print("Recording {} PID={}, 8M/64MiB, verified RX=256KiB".format(mode, child.pid))
            wait_marker(done, child, args.input_timeout, "Input phase")
        except BaseException as exc:
            failure = exc
        finally:
            # Only a successful -start grants ownership. Do not use -cancel.
            if started:
                try:
                    stop = subprocess.run(["wpr", "-stop", str(trace_path)], env=env,
                                          cwd=str(REPO_ROOT), stdout=stdout, stderr=stderr,
                                          timeout=120,
                                          creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                    if stop.returncode:
                        raise RuntimeError("WPR stop failed: " + str(stop.returncode))
                except BaseException as exc:
                    cleanup_errors.append(exc)
            # Stop failures must never skip release or child cleanup.
            try:
                release.write_text("release\n", encoding="ascii")
            except BaseException as exc:
                cleanup_errors.append(exc)
            try:
                if done.exists() and release.exists():
                    try:
                        child.wait(timeout=args.result_timeout)
                    except subprocess.TimeoutExpired:
                        raise RuntimeError("Benchmark result timed out")
                elif child.poll() is None:
                    child.kill()
            except BaseException as exc:
                cleanup_errors.append(exc)
            finally:
                try:
                    if child.poll() is None:
                        child.kill()
                    # Reap even when the process exited before the handshake.
                    child.wait(timeout=5)
                except BaseException as exc:
                    cleanup_errors.append(exc)
        if failure is not None:
            for error in cleanup_errors:
                print("Cleanup error: " + str(error), file=sys.stderr)
            raise failure
        if cleanup_errors:
            raise RuntimeError("; ".join(str(error) for error in cleanup_errors))
        if child.returncode:
            raise RuntimeError("Benchmark failed: " + str(child.returncode))
    with result_path.open(encoding="utf-8-sig") as stream:
        result = json.load(stream)
    if not isinstance(result, dict) or result.get("pass") is not True \
            or result.get("driverQueueExclusionConfirmed") is not True:
        raise RuntimeError("Profiled input completeness failed")
    print("Captured {}; frames={}, loss={}".format(
        trace_path, result.get("validFrames"), result.get("missingFrames")))


def main(argv=None):
    args = parser().parse_args(argv)
    # --help remains portable. Everything that launches tools follows this guard.
    if sys.platform != "win32":
        print("error: Windows is required for WPR CPU profiling", file=sys.stderr)
        return 1
    try:
        args.executable_path = args.executable_path.resolve()
        args.output_directory = args.output_directory.resolve()
        if not args.executable_path.is_file():
            raise RuntimeError("Benchmark executable does not exist: " + str(args.executable_path))
        env = os.environ.copy()
        env["PATH"] = str(args.qt_prefix / "bin") + os.pathsep + env.get("PATH", "")
        env["QT_PLUGIN_PATH"] = str(args.qt_prefix / "plugins")
        env["NOVATERM_RHI_API"] = "d3d11"
        env["QT_WIDGETS_RHI"] = "1"
        env.pop("QT_QPA_PLATFORM", None)
        require_idle_wpr(env)
        args.output_directory.mkdir(parents=True, exist_ok=True)
        # Run verifier self-test before any benchmark can open the serial ports.
        with (args.output_directory / "self-test.stdout.txt").open("wb") as stdout, \
                (args.output_directory / "self-test.stderr.txt").open("wb") as stderr:
            tested = subprocess.run([str(args.executable_path), "--self-test"], env=env,
                                    cwd=str(REPO_ROOT), stdout=stdout, stderr=stderr,
                                    timeout=30,
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if tested.returncode:
            raise RuntimeError("Verifier self-test failed: " + str(tested.returncode))
        for mode in args.modes:
            profile_mode(args, mode, env)
        return 0
    except KeyboardInterrupt:
        print("error: Profiling interrupted", file=sys.stderr)
        return 130
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print("error: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
