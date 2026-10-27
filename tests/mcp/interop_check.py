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
import uuid

ROOT = Path(__file__).resolve().parents[2]
_DEPS = str(ROOT / "build/mcp-test-deps")
site.addsitedir(_DEPS)
# site.addsitedir 把目录追加到 sys.path 末尾，系统自带的同名包会被抢先导入
# （本机 jsonschema 来自 /usr/lib/python3/dist-packages），导致 SDK 的 schema
# 校验器签名不匹配。提到最前，让 tests/mcp/requirements.txt 锁定的版本生效。
if _DEPS in sys.path:
    sys.path.remove(_DEPS)
sys.path.insert(0, _DEPS)
from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client
import httpx
from jsonschema import Draft202012Validator


async def check(config: dict) -> None:
    settings = config["mcpServers"]["novaterm"]
    async with streamablehttp_client(settings["url"], headers=settings["headers"]) as (read, write, _):
        async with ClientSession(read, write) as session:
            initialization = await session.initialize()
            assert initialization.protocolVersion == "2025-11-25"
            catalog = await session.list_tools()
            assert len(catalog.tools) == 21
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

            assert (await call("novaterm_list_captures", {}))["data"]["captures"] == []
            await check_target_connections(call)
            capture = "11111111-1111-4111-8111-111111111111"
            assert not (await call("novaterm_read_history", {"captureId": capture}))["ok"]
            assert not (await call("novaterm_search_history", {"captureId": capture, "query": "error"}))["ok"]

            sessions = (await call("novaterm_list_sessions", {}))["data"]["sessions"]
            assert len(sessions) == 1
            identity = {key: sessions[0][key] for key in ("sessionId", "epoch")}
            short = await call("novaterm_read_context", {**identity, "maxBytes": 1})
            assert short["data"]["outputTruncated"] and short["data"]["nextToken"] is None
            snapshot = (await call("novaterm_read_context", identity))["data"]
            result = await call("novaterm_search_context", {**identity,
                "captureId": snapshot["captureId"], "query": "ERROR"})
            assert result["data"]["matches"]
            async with streamablehttp_client(settings["url"], headers=settings["headers"]) as (other_read, other_write, _):
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
            await check_workflow(call, identity)
    await check_modern(settings)
    print("PASS: official MCP SDK 2025 + raw 2026 HTTP, 21 schemas, saved-target directory/open/query/replay/close-refusal, history/terminal contracts and B3a wait/start/query/stop/replay")


async def check_target_connections(call) -> None:
    listed = await call("novaterm_list_targets", {})
    assert listed["ok"] and len(listed["data"]["targets"]) == 1
    target = listed["data"]["targets"][0]
    arguments = {key: target[key] for key in ("targetId", "configVersion")}
    arguments["operationId"] = str(uuid.uuid4())
    missing = await call("novaterm_query_target", arguments)
    assert not missing["ok"] and missing["error"]["code"] == "TARGET_OPERATION_NOT_AVAILABLE"
    opened = await call("novaterm_open_target", arguments)
    assert opened["ok"] and opened["data"]["reused"] and not opened["data"]["created"]
    assert opened["data"]["state"] == "running"
    for name in ("novaterm_open_target", "novaterm_query_target"):
        result = await call(name, arguments)
        assert result["ok"] and result["data"] == opened["data"]
    rejected = await call("novaterm_open_target", {**arguments, "configVersion": str(uuid.uuid4())})
    assert not rejected["ok"] and rejected["error"]["code"] == "TARGET_OPEN_PERMISSION_REQUIRED"
    refused_close = await call("novaterm_close_target", {**arguments, "epoch": opened["data"]["epoch"]})
    assert not refused_close["ok"] and refused_close["error"]["code"] == "TARGET_CLOSE_NOT_ALLOWED"
    still_open = await call("novaterm_query_target", arguments)
    assert still_open["ok"] and still_open["data"] == opened["data"]


async def check_workflow(call, identity: dict) -> None:
    baseline = await call("novaterm_wait_text", {**identity, "source": "log", "texts": ["SDK_READY"]})
    assert baseline["ok"] and baseline["data"]["status"] == "timeout"
    assert not baseline["data"]["matched"]
    screen = await call("novaterm_wait_text", {**identity, "source": "screen", "start": "current",
        "texts": ["SDK_READY"], "waitMs": 1000})
    assert screen["ok"] and screen["data"]["status"] == "matched"
    assert not screen["data"]["provesSuccess"]
    observed = await call("novaterm_read_context", {**identity, "responseMode": "compact"})
    ordinary_id = str(uuid.uuid4())
    ordinary_actions = [{"type": "text", "text": "interop-ordinary"}]
    ordinary_args = {**identity, "inputToken": observed["data"]["inputToken"],
        "operationId": ordinary_id, "actions": ordinary_actions, "waitMs": 0}
    assert (await call("novaterm_interact_terminal", ordinary_args))["ok"]
    fresh = await call("novaterm_read_context", {**identity, "responseMode": "compact"})
    rejected = await call("novaterm_start_task", {**identity,
        "inputToken": fresh["data"]["inputToken"], "operationId": ordinary_id, "actions": ordinary_actions})
    assert not rejected["ok"] and rejected["error"]["code"] == "INPUT_SEQUENCE_CONFLICT", rejected
    assert (await call("novaterm_interact_terminal", ordinary_args))["ok"]
    observed = await call("novaterm_read_context", {**identity, "responseMode": "compact"})
    operation = str(uuid.uuid4())
    args = {**identity, "inputToken": observed["data"]["inputToken"], "operationId": operation,
        "actions": [{"type": "text", "text": "interop-task"}], "timeoutMs": 600000}
    started = await call("novaterm_start_task", args)
    assert started["ok"], started
    task = started["data"]["taskId"]
    retried = await call("novaterm_start_task", args)
    assert retried["ok"] and retried["data"]["taskId"] == task
    queried = await call("novaterm_query_task", {**identity, "operationId": operation,
        "sinceToken": started["data"]["baseline"]["nextToken"]})
    assert queried["ok"] and queried["data"]["taskId"] == task
    assert queried["data"]["exitCode"] is None and not queried["data"]["terminationConfirmed"]
    stop_args = {**identity, "taskId": task, "operationId": str(uuid.uuid4())}
    stopped = await call("novaterm_stop_task", stop_args)
    assert stopped["ok"] and stopped["data"]["stopRequested"], stopped
    stopped_again = await call("novaterm_stop_task", stop_args)
    assert stopped_again["ok"] and stopped_again["data"]["taskId"] == task


async def check_modern(settings: dict) -> None:
    request_id = 0
    client = httpx.AsyncClient(timeout=8)

    async def rpc(method: str, params: dict) -> dict:
        nonlocal request_id
        request_id += 1
        request = {"jsonrpc": "2.0", "id": request_id,
                   "method": method, "params": params}
        headers = {**settings["headers"], "Accept": "application/json, text/event-stream",
                   "MCP-Protocol-Version": params["_meta"]["io.modelcontextprotocol/protocolVersion"],
                   "Mcp-Method": method}
        if method == "tools/call":
            headers["Mcp-Name"] = params["name"]
        response = await client.post(settings["url"], json=request, headers=headers)
        if response.headers.get("content-type", "").startswith("text/event-stream"):
            messages = [json.loads(line[5:].strip()) for line in response.text.splitlines()
                        if line.startswith("data:")]
            assert messages, "HTTP SSE stream closed before a result"
            result = messages[-1]
        else:
            result = response.json()
        assert result.get("id") == request_id, result
        return result

    def metadata() -> dict:
        return {
            "io.modelcontextprotocol/protocolVersion": "2026-07-28",
            "io.modelcontextprotocol/clientInfo": {"name": "NovaTerm interop", "version": "1"},
            "io.modelcontextprotocol/clientCapabilities": {
                "elicitation": {"form": {}}},
        }

    try:
        discovered = await rpc("server/discover", {"_meta": metadata()})
        discovery = discovered["result"]
        assert discovery["resultType"] == "complete"
        assert "2026-07-28" in discovery["supportedVersions"]
        assert discovery["_meta"]["io.modelcontextprotocol/serverInfo"]["name"] == "novaterm"
        unsupported_meta = metadata()
        unsupported_meta["io.modelcontextprotocol/protocolVersion"] = "2099-01-01"
        unsupported = await rpc("tools/list", {"_meta": unsupported_meta})
        assert unsupported["error"]["code"] == -32022
        assert "2026-07-28" in unsupported["error"]["data"]["supported"]
        assert unsupported["error"]["data"]["requested"] == "2099-01-01"
        listed = await rpc("tools/list", {"_meta": metadata()})
        assert listed["result"]["resultType"] == "complete"
        assert len(listed["result"]["tools"]) == 21
        schemas = {tool["name"]: tool["outputSchema"] for tool in listed["result"]["tools"]}
        sessions = await rpc("tools/call", {"name": "novaterm_list_sessions",
            "arguments": {}, "_meta": metadata()})
        assert sessions["result"]["resultType"] == "complete"
        identity = sessions["result"]["structuredContent"]["data"]["sessions"][0]
        context = await rpc("tools/call", {"name": "novaterm_read_context",
            "arguments": {"sessionId": identity["sessionId"], "epoch": identity["epoch"]},
            "_meta": metadata()})
        assert context["result"]["resultType"] == "complete"
        assert context["result"]["structuredContent"]["ok"]
        async def call(name: str, arguments: dict) -> dict:
            result = await rpc("tools/call", {"name": name, "arguments": arguments, "_meta": metadata()})
            payload = result["result"]["structuredContent"]
            Draft202012Validator(schemas[name]).validate(payload)
            assert json.loads(result["result"]["content"][0]["text"]) == payload
            return payload
        await check_target_connections(call)
        await check_workflow(call, {key: identity[key] for key in ("sessionId", "epoch")})
    finally:
        await client.aclose()


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
