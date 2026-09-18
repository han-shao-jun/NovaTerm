"""同机 MCP 开/关吞吐对照；四个官方 SDK 客户端每个以 10 Hz 读取。"""
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
site.addsitedir(str(ROOT / "build/mcp-test-deps"))
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

FIXTURE_MASK = 0
CLIENT_MASK = 0


def isolate_cpus() -> None:
    """把负载进程和 SDK 客户端分配到不同 CPU，减少混合核心调度对 A/B 的干扰。"""
    global FIXTURE_MASK, CLIENT_MASK
    if os.name != "nt":
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
            for _ in range(4):
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
                await asyncio.sleep(0.1)

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
        mode = "--perf-fixture" if enabled else "--perf-baseline"
        fixture = subprocess.Popen([executable, mode, str(config)], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        try:
            if FIXTURE_MASK:
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                if not kernel.SetProcessAffinityMask(ctypes.c_void_p(int(fixture._handle)), ctypes.c_size_t(FIXTURE_MASK)):
                    raise RuntimeError("Cannot apply the declared fixture CPU affinity")
            until = time.monotonic() + 15
            while not config.exists() and time.monotonic() < until:
                if fixture.poll() is not None:
                    raise RuntimeError("Performance fixture exited during setup")
                time.sleep(0.02)
            return asyncio.run(workload(config, enabled))
        finally:
            config.with_name(config.name + ".stop").write_text("stop")
            try:
                fixture.wait(timeout=5)
            except subprocess.TimeoutExpired:
                fixture.kill()
                fixture.wait()


def main():
    isolate_cpus()
    executable = str(Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/Release/bin/novaterm_mcp_tests.exe"))
    results = []
    for _ in range(3):
        for enabled in (False, True):
            result = run(executable, enabled)
            results.append(result)
            print(json.dumps(result, ensure_ascii=True), flush=True)
    baseline = statistics.median(r["throughputMiBps"] for r in results if not r["mcpEnabled"])
    loaded = statistics.median(r["throughputMiBps"] for r in results if r["mcpEnabled"])
    summary = {"baselineMedianMiBps": baseline, "mcpMedianMiBps": loaded,
               "throughputDropPercent": (baseline - loaded) / baseline * 100,
               "fixtureCpuMask": FIXTURE_MASK, "clientCpuMask": CLIENT_MASK, "runs": results}
    path = ROOT / "build/p8-performance.json"
    path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Median throughput: {baseline:.2f} -> {loaded:.2f} MiB/s; drop {summary['throughputDropPercent']:.2f}%")


if __name__ == "__main__":
    main()
