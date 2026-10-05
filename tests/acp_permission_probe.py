# SPDX-License-Identifier: MIT
# Exercise the ACP permission boundary with a local mock provider.
import json
import os
import select
import subprocess
import sys
import tempfile
import time
from pathlib import Path

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1])
fixtures = Path.cwd()


def response(message, reason='stop'):
    return {'id': 'mock', 'object': 'chat.completion', 'created': 1735689600,
            'model': 'mock-model', 'choices': [{'index': 0, 'message': message,
                                               'finish_reason': reason}]}


def run_case(kind, decision):
    project = Path(tempfile.mkdtemp(prefix='permission-', dir=fixtures))
    marker = project / 'marker.txt'
    if kind == 'agent':
        name = 'agent'
        args = {}
    elif kind == 'shell':
        name = 'shell'
        args = {'command': 'printf executed > marker.txt'}
    else:
        name = 'write_file'
        args = {'path': 'marker.txt', 'content': 'executed'}
    tool = {'id': 'permission_call', 'type': 'function',
            'function': {'name': name, 'arguments': json.dumps(args)}}
    scenario = project / 'scenario.json'
    if kind == 'agent':
        scenario.write_text((root / 'tests/scenarios/agent_asks_user.json').read_text())
    else:
        scenario.write_text(json.dumps({'steps': [
            {'response': response({'role': 'assistant', 'content': None,
                                   'tool_calls': [tool]}, 'tool_calls')},
            {'response': response({'role': 'assistant', 'content': 'done'})}]}))
    server = subprocess.Popen(['python3', str(root / 'tests/mock/mock_provider.py'),
                               str(scenario), str(project)], stderr=subprocess.PIPE)
    peer = None
    try:
        deadline = time.monotonic() + 20
        while not (project / 'port').exists():
            if time.monotonic() >= deadline or server.poll() is not None:
                raise RuntimeError('mock provider did not start')
        port = (project / 'port').read_text().strip()
        api = 'responses' if kind == 'agent' else 'chat-completions'
        endpoint = '/v1/responses' if kind == 'agent' else '/v1/chat/completions'
        (project / 'config.yaml').write_text(
            f'api: {api}\n'
            f'api_url: http://127.0.0.1:{port}{endpoint}\n'
            'auth: api-key\nno_auth: true\ndisplay:\n  stream: false\n')
        subprocess.run([binary, 'init'], cwd=project, check=True, capture_output=True)
        peer = subprocess.Popen([binary, 'acp'], cwd=project, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        buffer = bytearray()
        frames = []

        def send(frame):
            peer.stdin.write((json.dumps({'jsonrpc': '2.0', **frame}) + '\n').encode())
            peer.stdin.flush()

        def next_frame():
            nonlocal buffer
            limit = time.monotonic() + 25
            while time.monotonic() < limit:
                if b'\n' in buffer:
                    line, _, rest = buffer.partition(b'\n')
                    buffer = bytearray(rest)
                    frame = json.loads(line)
                    frames.append(frame)
                    return frame
                if peer.poll() is not None:
                    raise RuntimeError(f'engine exited: {peer.stderr.read()!r}')
                ready, _, _ = select.select([peer.stdout], [], [], limit - time.monotonic())
                if ready:
                    buffer.extend(os.read(peer.stdout.fileno(), 65536))
            raise TimeoutError(frames)

        def until_id(target):
            while True:
                frame = next_frame()
                if frame.get('id') == target:
                    return frame

        send({'id': 1, 'method': 'initialize',
              'params': {'protocolVersion': 1, 'clientCapabilities': {}}})
        assert until_id(1).get('result', {}).get('protocolVersion') == 1
        send({'id': 2, 'method': 'session/new',
              'params': {'cwd': str(project), 'mcpServers': []}})
        session = until_id(2)['result']['sessionId']
        send({'id': 3, 'method': 'session/prompt',
              'params': {'sessionId': session,
                         'prompt': [{'type': 'text', 'text': 'run the tool'}]}})
        while True:
            frame = next_frame()
            if frame.get('id') == 3:
                raise AssertionError(f'prompt finished before permission: {frame}')
            if frame.get('method') == 'session/request_permission':
                break
        params = frame['params']
        assert params['sessionId'] == session
        assert params['toolCall']['toolCallId'] == ('c1' if kind == 'agent' else 'permission_call')
        assert params['toolCall']['name'] == name
        assert [o['kind'] for o in params['options']] == ['allow_once', 'reject_once']
        assert not marker.exists(), 'tool ran before permission'
        if decision == 'deny_nested':
            outcome = {'outcome': 'selected', 'optionId': 'allow-once'}
        elif decision == 'cancel':
            send({'method': 'session/cancel', 'params': {'sessionId': session}})
            outcome = {'outcome': 'cancelled'}
        elif decision == 'malformed':
            outcome = {'outcome': 'selected', 'optionId': 'unknown'}
        else:
            outcome = {'outcome': 'selected',
                       'optionId': 'allow-once' if decision == 'allow' else 'deny-once'}
        send({'id': frame['id'], 'result': {'outcome': outcome}})
        if decision == 'deny_nested':
            while True:
                frame = next_frame()
                assert frame.get('method') != 'elicitation/create', 'nested tool ran without permission'
                if frame.get('method') == 'session/request_permission':
                    break
                assert frame.get('id') != 3, 'nested permission was not requested'
            assert frame['params']['toolCall']['name'] == 'ask_user', frame
            send({'id': frame['id'], 'result': {'outcome': {
                'outcome': 'selected', 'optionId': 'deny-once'}}})
        done = until_id(3)
        assert 'result' in done or 'error' in done, done
        if decision == 'deny_nested':
            assert not any(f.get('method') == 'elicitation/create' for f in frames)
            print(kind, decision, 'passed')
            return
        updates = [f['params']['update'] for f in frames if f.get('method') == 'session/update'
                   and f['params']['update']['sessionUpdate'] in ('tool_call', 'tool_call_update')]
        statuses = [u['status'] for u in updates]
        if decision == 'allow':
            assert marker.read_text() == 'executed'
            assert statuses == ['pending', 'in_progress', 'completed'], statuses
        else:
            assert not marker.exists(), f'{kind} ran after {decision}'
            assert statuses == ['pending', 'failed'], statuses
            if decision != 'cancel':
                assert 'permission denied' in updates[-1]['content'][0]['content']['text']
        print(kind, decision, 'passed')
    finally:
        if peer is not None:
            if peer.poll() is None:
                peer.stdin.close()
                try:
                    peer.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    peer.terminate()
                    peer.wait(timeout=5)
        server.terminate()
        server.wait(timeout=5)


for case in [('write', 'deny'), ('write', 'allow'), ('shell', 'deny'),
             ('write', 'malformed'), ('write', 'cancel'), ('agent', 'deny_nested')]:
    run_case(*case)
