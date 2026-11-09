"""复用当前 Ninja 构建的生产对象，独立验证真实主窗口析构；不启动网络连接。"""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    build = Path(sys.argv[1]).resolve()
    commands = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", "NovaTerm"], text=True).splitlines()
    compile_command = next(shlex.split(line) for line in commands
                           if " -c " in line and line.endswith("/src/main.cpp"))
    link_command = shlex.split(commands[-1].split(" && ")[1])
    with tempfile.TemporaryDirectory(prefix="mainwindow-shutdown-") as temp:
        directory = Path(temp)
        obj, executable = directory / "probe.o", directory / "probe"
        compile_command[compile_command.index("-o") + 1] = str(obj)
        compile_command[compile_command.index("-c") + 1] = str(ROOT / "tests/ui/MainWindowShutdownProbe.cpp")
        if "-MF" in compile_command:
            compile_command[compile_command.index("-MF") + 1] = str(directory / "probe.d")
        subprocess.run(compile_command, cwd=build, check=True)
        link_command[link_command.index("CMakeFiles/NovaTerm.dir/src/main.cpp.o")] = str(obj)
        link_command[link_command.index("-o") + 1] = str(executable)
        subprocess.run(link_command, cwd=build, check=True)
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen",
                   XDG_CONFIG_HOME=str(directory / "config"),
                   XDG_DATA_HOME=str(directory / "data"),
                   XDG_CACHE_HOME=str(directory / "cache"))
        failed = False
        for mode in ("--sftp", "--monitor"):
            result = subprocess.run([str(executable), mode], cwd=directory, env=env,
                                    capture_output=True, text=True, timeout=30)
            print(f"{mode}: exit={result.returncode}\n{result.stdout}", flush=True)
            print(result.stderr, file=sys.stderr, end="")
            failed |= result.returncode != 0 or "shutdown completed" not in result.stdout
        return int(failed)


if __name__ == "__main__":
    sys.exit(main())
