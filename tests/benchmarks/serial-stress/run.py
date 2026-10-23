#!/usr/bin/env python3
"""Run the serial stress acceptance matrix with Python 3.9+.

Command planning and process orchestration are portable. The bundled native
serial_stress fixture currently uses Win32 CPU/RSS APIs and only builds on
Windows; this runner does not remove that native limitation.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[3]


def parse_arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("Smoke", "Diagnostics", "ChunkControls", "LongCases", "TimingControls",
                 "StableRawCase", "StableTerminalCase", "DriverQueueProbe", "LargeQueueCases"):
        option = re.sub(r"(?<!^)(?=[A-Z])", "-", name).lower()
        parser.add_argument("--" + option, "-" + name, action="store_true")
    for option, legacy, default in (
        ("output-directory", "OutputDirectory", None),
        ("qt-prefix", "QtPrefix", "C:/Programs/Qt/6.8.3/msvc2022_64" if sys.platform == "win32" else None),
        ("executable-path", "ExecutablePath", None),
        ("port-a", "PortA", "COM1"),
        ("port-b", "PortB", "COM2"),
    ):
        parser.add_argument("--" + option, "-" + legacy, default=default)
    return parser.parse_args(argv)


def result_group(args):
    # Retain the original grouping, including long-only cases in "matrix".
    if args.driver_queue_probe or args.large_queue_cases:
        return "large-queue"
    if args.timing_controls or args.stable_raw_case or args.stable_terminal_case:
        return "timing-controls"
    if args.chunk_controls:
        return "block-controls"
    if args.diagnostics:
        return "controls"
    return "smoke" if args.smoke else "matrix"


def child_environment(args):
    environment = os.environ.copy()
    if args.qt_prefix:
        prefix = Path(args.qt_prefix)
        bin_path = str(prefix / "bin")
        environment["PATH"] = bin_path + (os.pathsep + environment["PATH"] if environment.get("PATH") else "")
        environment["QT_PLUGIN_PATH"] = str(prefix / "plugins")
    if sys.platform == "win32":
        environment.setdefault("NOVATERM_RHI_API", "d3d11")
    environment.setdefault("QT_WIDGETS_RHI", "1")
    # The acceptance fixture measures real visible/hidden rendering.
    environment.pop("QT_QPA_PLATFORM", None)
    return environment


def port_slug(port):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", port).strip("._") or "port"


def matrix_commands(args, output):
    """Yield argument lists in the historical matrix order (without executable)."""
    directions = ((args.port_a, args.port_b), (args.port_b, args.port_a))

    def case(first, second, baud=8000000, seconds=20, mode="visible", pacing="paced",
             size=67108864, extra=(), suffix="", filename=None):
        if filename is None:
            filename = f"{baud}-{port_slug(first)}-{port_slug(second)}-{mode}{suffix}-{pacing}"
            if size is not None:
                filename += f"-{size}"
            filename += ".json"
        command = [first, second, str(baud), str(seconds), mode, pacing, str(output / filename)]
        if size is not None:
            command += ["--bytes", str(size)]
        return command + list(extra)

    stable = ("--stable-timers",)
    deferred = ("--deferred-verify",)
    if args.driver_queue_probe:
        yield case(*directions[0], seconds=1, size=1048576,
                   extra=("--driver-rx-queue", "262144") + stable + deferred,
                   filename="queue-probe.json")
    elif args.large_queue_cases:
        for first, second in directions:
            for mode in ("visible", "hidden", "raw"):
                name = f"8000000-{port_slug(first)}-{port_slug(second)}-{mode}-stable-deferred-paced-67108864-rx262144.json"
                yield case(first, second, mode=mode,
                           extra=("--driver-rx-queue", "262144") + stable + deferred, filename=name)
    elif args.stable_terminal_case:
        yield case(*directions[0], extra=stable + deferred, suffix="-stable-deferred")
    elif args.stable_raw_case:
        yield case(*directions[0], mode="raw", extra=stable, suffix="-stable")
    elif args.timing_controls:
        for mode in ("raw", "visible"):
            yield case(*directions[0], mode=mode, extra=stable if mode == "visible" else ())
        yield case(*directions[0], mode="raw", extra=stable, suffix="-stable")
        yield case(*directions[0], extra=stable + deferred, suffix="-stable-deferred")
    elif args.long_cases:
        for direction in directions:
            yield case(*direction)
    elif args.chunk_controls:
        for port in (args.port_a, args.port_b):
            yield ["--probe", port, str(output / f"driver-{port_slug(port)}.json")]
        for first, second in directions:
            for mode in ("visible", "raw"):
                name = f"8000000-{port_slug(first)}-{port_slug(second)}-{mode}-deferred-burst-8388608-chunk4096.json"
                yield case(first, second, mode=mode, pacing="burst", size=8388608,
                           extra=deferred + ("--tx-chunk", "4096"), filename=name)
    elif args.diagnostics:
        for direction in directions:
            for mode in ("visible", "raw"):
                yield case(*direction, mode=mode, pacing="burst", extra=deferred, suffix="-deferred")
    elif args.smoke:
        for mode in ("hidden", "visible"):
            yield case(*directions[0], baud=1000000, seconds=1, mode=mode,
                       size=None, filename=f"smoke-{mode}.json")
    else:
        for baud in (1000000, 3000000, 5000000, 8000000):
            for direction in directions:
                for size in (65536, 1048576, 8388608):
                    yield case(*direction, baud=baud, size=size)
                yield case(*direction, baud=baud, mode="hidden", size=8388608)
        for first, second in directions:
            yield case(first, second)
            # Historical burst filenames omit the byte count.
            name = f"8000000-{port_slug(first)}-{port_slug(second)}-visible-burst.json"
            yield case(first, second, pacing="burst", filename=name)


def main(argv=None):
    args = parse_arguments(argv)
    output = Path(args.output_directory) if args.output_directory else REPO_ROOT / "build" / "serial-stress-results" / result_group(args)
    executable = args.executable_path or str(REPO_ROOT / "build" / "serial-stress-native" / ("serial_stress.exe" if sys.platform == "win32" else "serial_stress"))
    environment = child_environment(args)
    try:
        output.mkdir(parents=True, exist_ok=True)
        if subprocess.run([executable, "--self-test"], env=environment).returncode != 0:
            print("Frame verifier self-test failed", file=sys.stderr)
            return 1
        failures = 0
        # Only the large-queue matrix treats exit 7 as a hard stop; queue probe
        # and all other scenarios retain ordinary failure counting.
        stop_on_queue_rejection = args.large_queue_cases and not args.driver_queue_probe
        for command in matrix_commands(args, output):
            print("Starting " + Path(command[2] if command[0] == "--probe" else command[6]).stem, flush=True)
            result = subprocess.run([executable, *command], env=environment)
            if stop_on_queue_rejection and result.returncode == 7:
                print("Driver queue enlargement rejected; stop matrix", file=sys.stderr)
                return 1
            if result.returncode != 0:
                failures += 1
        print(f"Matrix finished: failures={failures}")
        return failures
    except OSError as error:
        print(f"Serial stress runner failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
