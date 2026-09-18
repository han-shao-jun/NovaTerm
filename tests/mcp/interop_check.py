"""官方 MCP Python 客户端、完整输出 schema 和独立客户端的互操作检查。"""
from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path
import site
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
site.addsitedir(str(ROOT / "build/mcp-test-deps"))
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from jsonschema import Draft202012Validator


async def check(config: dict) -> None:
    settings = config["mcpServers"]["novaterm"]
    params = StdioServerParameters(command=settings["command"], args=settings["args"],
                                  env={**os.environ, **settings["env"]})
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            initialization = await session.initialize()
            assert initialization.protocolVersion == "2025-11-25"
            catalog = await session.list_tools()
            assert len(catalog.tools) == 5
            schemas = {tool.name: tool.outputSchema for tool in catalog.tools}
            for tool in catalog.tools:
                Draft202012Validator.check_schema(tool.inputSchema)
                Draft202012Validator.check_schema(tool.outputSchema)

            async def call(name: str, args: dict) -> dict:
                result = await session.call_tool(name, args)
                payload = result.structuredContent
                Draft202012Validator(schemas[name]).validate(payload)
                assert json.loads(result.content[0].text) == payload
                return payload

            sessions = (await call("novaterm_list_sessions", {}))["data"]["sessions"]
            assert len(sessions) == 1
            identity = {key: sessions[0][key] for key in ("sessionId", "epoch")}
            short = await call("novaterm_read_context", {**identity, "maxBytes": 1})
            assert short["data"]["outputTruncated"] and short["data"]["nextToken"] is None
            snapshot = (await call("novaterm_read_context", identity))["data"]
            result = await call("novaterm_search_context", {**identity,
                "captureId": snapshot["captureId"], "query": "ERROR"})
            assert result["data"]["matches"]
            async with stdio_client(params) as (other_read, other_write):
                async with ClientSession(other_read, other_write) as other:
                    await other.initialize()
                    independent = await other.call_tool("novaterm_read_context", identity)
                    assert independent.structuredContent["data"]["recentOutput"] == snapshot["recentOutput"]
            commands = (await call("novaterm_list_commands", identity))["data"]
            assert commands["executionEnabled"] and len(commands["commands"]) == 4
            command = commands["commands"][0]
            arguments = {**identity, "commandId": command["commandId"],
                "policyVersion": commands["policyVersion"], "commandTicket": command["commandTicket"], "arguments": {}}
            executed = await call("novaterm_execute_command", arguments)
            assert executed["ok"] and executed["data"]["terminationConfirmed"]
            assert await call("novaterm_execute_command", arguments) == executed
            rejected = await call("novaterm_read_context", {**identity, "command": "not allowed"})
            assert not rejected["ok"] and rejected["error"]["code"] == "INVALID_ARGUMENT"
            print("PASS: official MCP SDK, 5 tool schemas, bounded reads, search, 2 clients, execution and replay")


def main() -> None:
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/Release/bin/novaterm_mcp_tests.exe")
    with tempfile.TemporaryDirectory(dir=ROOT / "build", prefix="mcp-interop-") as temporary:
        config = Path(temporary) / "client.json"
        fixture = subprocess.Popen([str(executable), "--interop-fixture", str(config)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        try:
            until = time.monotonic() + 10
            while not config.exists() and time.monotonic() < until:
                if fixture.poll() is not None:
                    raise RuntimeError("Interop fixture exited before startup")
                time.sleep(0.02)
            asyncio.run(check(json.loads(config.read_text(encoding="utf-8"))))
        finally:
            config.with_name(config.name + ".stop").write_text("stop", encoding="utf-8")
            try:
                fixture.wait(timeout=5)
            except subprocess.TimeoutExpired:
                fixture.kill()
                fixture.wait()


if __name__ == "__main__":
    main()
