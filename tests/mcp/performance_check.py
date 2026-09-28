"""同机 MCP 开/关吞吐对照；四个官方 SDK 客户端按固定频率读取，同时报告 CPU 帧时间。

需要可用的图形会话：夹具要实例化 QRhiWidget 才能测量 render() 的 CPU 帧时间。
offscreen 平台插件在本机拿不到 QRhi，因此脚本会把子进程切到 xcb + OpenGL；
拿不到设备时夹具以专用退出码失败并打印诊断，不会把「零帧」当成极好成绩。
"""
from __future__ import annotations
import asyncio
from contextlib import AsyncExitStack
import json
import os
from pathlib import Path
import site
import statistics
import subprocess
import sys
import tempfile
import time
import ctypes

ROOT = Path(__file__).resolve().parents[2]
_DEPS = str(ROOT / "build/mcp-test-deps")
site.addsitedir(_DEPS)
# site.addsitedir 把目录追加到 sys.path 末尾，系统自带的同名包会被抢先导入
# （本机 jsonschema 来自 /usr/lib/python3/dist-packages），导致 SDK 的 schema
# 校验器签名不匹配。提到最前，让 tests/mcp/requirements.txt 锁定的版本生效。
if _DEPS in sys.path:
    sys.path.remove(_DEPS)
sys.path.insert(0, _DEPS)
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

FIXTURE_MASK: object = 0
CLIENT_MASK: object = 0
CLIENT_COUNT = int(os.environ.get("NOVATERM_MCP_PERF_CLIENTS", "4"))
POLL_INTERVAL = float(os.environ.get("NOVATERM_MCP_PERF_INTERVAL", "0.1"))
# 夹具以这些退出码报告「测不出帧指标」，与真实测量失败区分开。
FIXTURE_NO_DEVICE = 3
FIXTURE_NO_FRAMES = 4


def fixture_environment() -> dict:
    """子进程环境：有显示服务时切到 xcb + OpenGL，否则保留调用方设置并让夹具自己报错。"""
    env = dict(os.environ)
    if not sys.platform.startswith("win") and (env.get("DISPLAY") or env.get("WAYLAND_DISPLAY")):
        env["QT_QPA_PLATFORM"] = "xcb"
        env.setdefault("QT_WIDGETS_RHI", "1")
        env.setdefault("NOVATERM_RHI_API", "opengl")
    return env


def isolate_cpus() -> None:
    """把负载进程和 SDK 客户端分配到不同 CPU，减少混合核心调度对 A/B 的干扰。"""
    global FIXTURE_MASK, CLIENT_MASK
    if os.name != "nt":
        available = sorted(os.sched_getaffinity(0))
        if len(available) < 8:
            return
        fixture = set(available[:4])
        clients = set(available[4:])
        try:
            os.sched_setaffinity(0, clients)
        except OSError:
            return
        FIXTURE_MASK, CLIENT_MASK = sorted(fixture), sorted(clients)
        return
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    process = ctypes.c_size_t()
    system = ctypes.c_size_t()
    if not kernel.GetProcessAffinityMask(ctypes.c_void_p(-1), ctypes.byref(process), ctypes.byref(system)):
        return
    cpus = [index for index in range(64) if process.value & (1 << index)]
    if len(cpus) < 8:
        return
    FIXTURE_MASK = sum(1 << index for index in cpus[:4])
    CLIENT_MASK = process.value & ~FIXTURE_MASK
    if not kernel.SetProcessAffinityMask(ctypes.c_void_p(-1), ctypes.c_size_t(CLIENT_MASK)):
        FIXTURE_MASK = CLIENT_MASK = 0


async def workload(config: Path, enabled: bool) -> dict:
    async with AsyncExitStack() as stack:
        clients = []
        if enabled:
            settings = json.loads(config.read_text(encoding="utf-8"))["mcpServers"]["novaterm"]
            parameters = StdioServerParameters(command=settings["command"], args=settings["args"],
                env={**os.environ, **settings["env"]})
            for _ in range(CLIENT_COUNT):
                read, write = await stack.enter_async_context(stdio_client(parameters))
                client = await stack.enter_async_context(ClientSession(read, write))
                await client.initialize()
                result = await client.call_tool("novaterm_list_sessions", {})
                session = result.structuredContent["data"]["sessions"][0]
                clients.append((client, {k: session[k] for k in ("sessionId", "epoch")}))
        done = asyncio.Event()
        durations = []
        successful = []
        errors = {}

        async def poll(client, identity):
            while not done.is_set():
                start = time.perf_counter()
                response = await client.call_tool("novaterm_read_context", identity)
                duration = (time.perf_counter() - start) * 1000
                durations.append(duration)
                if response.isError:
                    code = response.structuredContent.get("error", {}).get("code", "UNKNOWN")
                    errors[code] = errors.get(code, 0) + 1
                else:
                    successful.append(duration)
                await asyncio.sleep(POLL_INTERVAL)

        config.with_name(config.name + ".start").write_text("start")
        tasks = [asyncio.create_task(poll(client, identity)) for client, identity in clients]
        result_file = config.with_name(config.name + ".result")
        until = time.monotonic() + 40
        try:
            while not result_file.exists() and time.monotonic() < until:
                await asyncio.sleep(0.02)
            result = json.loads(result_file.read_text(encoding="utf-8"))
        finally:
            done.set()
            await asyncio.gather(*tasks)
        result["rpcSamples"] = len(durations)
        result["successfulReads"] = len(successful)
        result["readErrors"] = errors
        if successful:
            result["successfulRpcP95Ms"] = sorted(successful)[max(0, (len(successful) * 95 + 99) // 100 - 1)]
        if durations:
            result["rpcP95Ms"] = sorted(durations)[max(0, (len(durations) * 95 + 99) // 100 - 1)]
        return result


def run(executable: str, enabled: bool) -> dict:
    with tempfile.TemporaryDirectory(dir=ROOT / "build", prefix="mcp-perf-") as folder:
        config = Path(folder) / "config.json"
        stderr_file = Path(folder) / "fixture-stderr.txt"
        mode = "--perf-fixture" if enabled else "--perf-baseline"
        with stderr_file.open("wb") as stderr_sink:
            fixture = subprocess.Popen([executable, mode, str(config)], stdout=subprocess.DEVNULL,
                stderr=stderr_sink, env=fixture_environment(),
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            try:
                if FIXTURE_MASK:
                    if os.name == "nt":
                        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                        if not kernel.SetProcessAffinityMask(ctypes.c_void_p(int(fixture._handle)),
                                                            ctypes.c_size_t(FIXTURE_MASK)):
                            raise RuntimeError("Cannot apply the declared fixture CPU affinity")
                    else:
                        os.sched_setaffinity(fixture.pid, set(FIXTURE_MASK))
                until = time.monotonic() + 15
                while not config.exists() and time.monotonic() < until:
                    if fixture.poll() is not None:
                        raise RuntimeError(describe_fixture_exit(fixture, stderr_file))
                    time.sleep(0.02)
                try:
                    return asyncio.run(workload(config, enabled))
                except (FileNotFoundError, json.JSONDecodeError) as error:
                    # 夹具在负载中途以专用退出码失败时不会写 .result，这里补上诊断。
                    code = fixture.poll()
                    if code:
                        raise RuntimeError(describe_fixture_exit(fixture, stderr_file)) from error
                    raise
            finally:
                config.with_name(config.name + ".stop").write_text("stop")
                try:
                    fixture.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    fixture.kill()
                    fixture.wait()


def describe_fixture_exit(fixture: subprocess.Popen, stderr_file: Path) -> str:
    """把夹具的专用退出码翻译成可操作的诊断；stderr 之前被重定向到文件。"""
    code = fixture.returncode
    detail = stderr_file.read_text(encoding="utf-8", errors="replace").strip()
    if code == FIXTURE_NO_DEVICE:
        return (f"夹具拿不到 QRhi 设备（退出码 {code}），CPU 帧时间无法测量。"
                f"请在有显示服务的会话中运行。stderr: {detail}")
    if code == FIXTURE_NO_FRAMES:
        return f"夹具负载期间零帧（退出码 {code}），CPU 帧时间无意义。stderr: {detail}"
    return f"性能夹具以退出码 {code} 结束。stderr: {detail}"


def main():
    isolate_cpus()
    default_exe = "novaterm_mcp_tests.exe" if os.name == "nt" else "novaterm_mcp_tests"
    executable = str(Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/Release/bin" / default_exe))
    results = []
    for _ in range(3):
        for enabled in (False, True):
            result = run(executable, enabled)
            results.append(result)
            print(json.dumps(result, ensure_ascii=True), flush=True)
    off = [r for r in results if not r["mcpEnabled"]]
    on = [r for r in results if r["mcpEnabled"]]
    baseline = statistics.median(r["throughputMiBps"] for r in off)
    loaded = statistics.median(r["throughputMiBps"] for r in on)
    baseline_frames = [r["cpuFrameP95Ns"] / 1.0e6 for r in off]
    loaded_frames = [r["cpuFrameP95Ns"] / 1.0e6 for r in on]
    spread = lambda xs: (max(xs) - min(xs)) if xs else 0.0
    # P95 取自渲染器最近 2048 帧的滚动窗口。样本帧数远低于窗口容量时，P95 实质是
    # 「第 9 大的那一帧」，抽样误差极大；此时 delta 不得作为验收依据，只如实报告
    # 两臂原值与离散度，让读者自行判断可辨识性（见 P8 §14.2 的口径说明）。
    min_frames = int(os.environ.get("NOVATERM_MCP_PERF_MIN_FRAMES", "1000"))
    enough = all(r["framesRendered"] >= min_frames for r in results)
    frame_delta = (statistics.median(loaded_frames) - statistics.median(baseline_frames)) if enough else None
    summary = {"baselineMedianMiBps": baseline, "mcpMedianMiBps": loaded,
               "throughputDropPercent": (baseline - loaded) / baseline * 100,
               "baselineCpuFrameP95Ms": statistics.median(baseline_frames),
               "mcpCpuFrameP95Ms": statistics.median(loaded_frames),
               "cpuFrameP95DeltaMs": frame_delta,
               "cpuFrameP95Reliable": enough,
               "cpuFrameP95Samples": [r["framesRendered"] for r in results],
               "baselineCpuFrameP95RawMs": baseline_frames,
               "mcpCpuFrameP95RawMs": loaded_frames,
               "baselineCpuFrameP95SpreadMs": spread(baseline_frames),
               "mcpCpuFrameP95SpreadMs": spread(loaded_frames),
               "minFramesForReliableP95": min_frames,
               "pollIntervalSeconds": POLL_INTERVAL, "clientCount": CLIENT_COUNT,
               "frameMetric": "TerminalRenderer::renderStatistics().cpuFrameP95Nanoseconds"
                              " (render() CPU time only, rolling last 2048 frames; does not"
                              " cover read_context projection/encoding on the GUI thread)",
               "fixtureCpuMask": FIXTURE_MASK, "clientCpuMask": CLIENT_MASK, "runs": results}
    path = ROOT / "build/p8-performance.json"
    path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Median throughput: {baseline:.2f} -> {loaded:.2f} MiB/s; drop {summary['throughputDropPercent']:.2f}%")
    print(f"CPU frame P95 raw (ms): baseline {baseline_frames} spread {spread(baseline_frames):.3f}"
          f" | mcp {loaded_frames} spread {spread(loaded_frames):.3f}")
    if enough:
        print(f"Median CPU frame P95: {summary['baselineCpuFrameP95Ms']:.3f} -> "
              f"{summary['mcpCpuFrameP95Ms']:.3f} ms; delta {summary['cpuFrameP95DeltaMs']:+.3f} ms")
    else:
        print("CPU frame P95 delta NOT reported: frames per run "
              f"{summary['cpuFrameP95Samples']} below the {min_frames}-frame reliability floor; "
              "P95 is a single order statistic of too few samples to judge a millisecond-scale delta.")


if __name__ == "__main__":
    main()
