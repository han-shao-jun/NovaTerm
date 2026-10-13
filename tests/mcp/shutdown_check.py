"""HTTP MCP 生命周期验收；仅创建自己的临时应用进程和本机 HTTP 连接。"""
import argparse
import http.client
import json
import os
import pathlib
import subprocess
import tempfile
import time
import urllib.parse


def connection(settings):
    url = urllib.parse.urlsplit(settings['url'])
    if url.scheme != 'http' or url.hostname not in ('127.0.0.1', 'localhost', '::1'):
        raise RuntimeError('Fixture endpoint must be loopback HTTP')
    return http.client.HTTPConnection(url.hostname, url.port, timeout=3), url.path or '/'


def post(settings, message, session=None, read=True):
    client, path = connection(settings)
    headers = {**settings['headers'], 'Content-Type': 'application/json',
               'Accept': 'application/json, text/event-stream'}
    if session:
        headers['Mcp-Session-Id'] = session
        headers['MCP-Protocol-Version'] = '2025-11-25'
    client.request('POST', path, json.dumps(message, separators=(',', ':')), headers)
    if not read:
        return client
    try:
        response = client.getresponse()
        body = response.read().decode()
        if response.getheader('Content-Type', '').startswith('text/event-stream'):
            messages = [json.loads(line[5:].strip()) for line in body.splitlines()
                        if line.startswith('data:')]
            result = messages[-1] if messages else {}
        else:
            result = json.loads(body) if body else {}
        return response.status, response.getheader('Mcp-Session-Id'), result
    finally:
        client.close()


def run_case(fixture_exe, environment, directory_root, name, pending=False, crash=False):
    fixture = exchange = None
    with tempfile.TemporaryDirectory(prefix='mcp-shutdown-', dir=directory_root) as directory:
        if not pathlib.Path(directory).resolve().is_relative_to(directory_root):
            raise RuntimeError('Temporary directory outside verification root')
        config_file = pathlib.Path(directory) / 'configuration.json'
        try:
            fixture = subprocess.Popen([str(fixture_exe), '--shutdown-fixture', str(config_file)],
                env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            deadline = time.monotonic() + 5
            while not config_file.exists() and time.monotonic() < deadline:
                if fixture.poll() is not None:
                    raise RuntimeError('Fixture exited before ready')
                time.sleep(0.02)
            # 令牌只从临时配置读取并用于 Authorization，不写入日志。
            settings = json.loads(config_file.read_text(encoding='utf-8'))['mcpServers']['novaterm']
            status, session, initialized = post(settings, {'jsonrpc': '2.0', 'id': 1,
                'method': 'initialize', 'params': {'protocolVersion': '2025-11-25',
                'capabilities': {}, 'clientInfo': {'name': 'HTTP shutdown check', 'version': '1'}}})
            if status != 200 or not session or initialized.get('id') != 1:
                raise RuntimeError('HTTP initialize failed')
            if post(settings, {'jsonrpc': '2.0', 'method': 'notifications/initialized'}, session)[0] != 202:
                raise RuntimeError('HTTP initialized notification failed')
            _, _, response = post(settings, {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/call',
                'params': {'name': 'novaterm_list_sessions', 'arguments': {}}}, session)
            sessions = response['result']['structuredContent']['data']['sessions']
            if pending:
                identity = {key: sessions[0][key] for key in ('sessionId', 'epoch')}
                _, _, response = post(settings, {'jsonrpc': '2.0', 'id': 3, 'method': 'tools/call',
                    'params': {'name': 'novaterm_list_commands', 'arguments': identity}}, session)
                catalog = response['result']['structuredContent']['data']
                command = catalog['commands'][0]
                arguments = {**identity, 'commandId': command['commandId'],
                    'policyVersion': catalog['policyVersion'], 'commandTicket': command['commandTicket'],
                    'arguments': {}}
                exchange = post(settings, {'jsonrpc': '2.0', 'id': 4, 'method': 'tools/call',
                    'params': {'name': 'novaterm_execute_command', 'arguments': arguments}}, session, read=False)
                # 夹具保留未完成命令；客户端保持 HTTP 连接并不读取 SSE。
                time.sleep(0.2)
                if fixture.poll() is not None:
                    raise RuntimeError('Fixture exited before shutdown')
            began = time.monotonic()
            if crash:
                fixture.kill()
            else:
                pathlib.Path(str(config_file) + '.stop').touch()
            code = fixture.wait(timeout=4)
            elapsed = time.monotonic() - began
            if (not crash and code != 0) or elapsed >= 3:
                raise RuntimeError(f'Unexpected application shutdown: exit={code}, elapsed={elapsed:.3f}')
            # 已停止应用不再提供 HTTP 服务；不得启动或重放旧命令。
            try:
                post(settings, {'jsonrpc': '2.0', 'id': 5, 'method': 'ping'}, session)
            except (OSError, http.client.HTTPException):
                pass
            else:
                raise RuntimeError('HTTP endpoint still accepts requests after application exit')
            if exchange is not None:
                try:
                    response = exchange.getresponse()
                    response.read()
                except (OSError, http.client.HTTPException):
                    pass  # 断流是传输失败，不伪造成功或命令执行结果。
            print(f'{name}: PASS elapsed_ms={elapsed * 1000:.0f} http_closed=1', flush=True)
        finally:
            if exchange is not None:
                exchange.close()
            if fixture is not None and fixture.poll() is None:
                fixture.kill()
                fixture.wait(timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', required=True, type=pathlib.Path)
    parser.add_argument('--qt-prefix', type=pathlib.Path)
    args = parser.parse_args()
    fixture_exe = args.fixture.resolve(strict=True)
    directory_root = (fixture_exe.parent.parent / 'mcp-shutdown-check').resolve()
    directory_root.mkdir(exist_ok=True)
    environment = dict(os.environ, QT_QPA_PLATFORM='offscreen')
    if args.qt_prefix:
        environment['PATH'] = str(args.qt_prefix / 'bin') + os.pathsep + environment['PATH']
        environment['QT_PLUGIN_PATH'] = str(args.qt_prefix / 'plugins')
    run_case(fixture_exe, environment, directory_root, 'application exits, idle HTTP session')
    run_case(fixture_exe, environment, directory_root, 'application exits, unread pending SSE', pending=True)
    run_case(fixture_exe, environment, directory_root, 'application crashes, pending SSE', pending=True, crash=True)


if __name__ == '__main__':
    main()
