# SPDX-License-Identifier: MIT
"""Exercise automatic route, provenance, and budget with a local provider."""
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


def exercise(budget, should_succeed, missing_usage=False,
             manual_override=False, endpoint_mismatch=False):
    project = Path(tempfile.mkdtemp(prefix='route-', dir=Path.cwd()))
    scenario = project / 'scenario.json'
    first_response = {
        'id': 'route-response', 'object': 'chat.completion', 'created': 1735689600,
        'model': 'deep-wire', 'choices': [{'index': 0, 'message': {
            'role': 'assistant', 'content': 'The build is repaired.'},
            'finish_reason': 'stop'}],
        'usage': {'prompt_tokens': 50, 'completion_tokens': 20,
                  'total_tokens': 70}}
    if missing_usage:
        del first_response['usage']
    scenario.write_text(json.dumps({'steps': [
        {'response': first_response},
        {'response': {**first_response, 'model': 'fast-wire',
                      'id': 'route-response-2'}},
        {'response': {**first_response, 'model': 'fast-wire',
                      'id': 'route-response-3'}},
        {'response': {**first_response, 'model': 'fast-wire',
                      'id': 'route-response-4'}}]}))
    server = subprocess.Popen(['python3', str(root / 'tests/mock/mock_provider.py'),
                               str(scenario), str(project)], stderr=subprocess.PIPE)
    peer = None
    try:
        deadline = time.monotonic() + 20
        while not (project / 'port').exists():
            if time.monotonic() >= deadline or server.poll() is not None:
                raise RuntimeError('mock provider did not start')
        port = (project / 'port').read_text().strip()
        (project / 'config.yaml').write_text(
            'model: route-fast\napi: chat-completions\n'
            f'api_url: http://127.0.0.1:{port}/v1/chat/completions\n'
            'auth: api-key\nno_auth: true\ndisplay:\n  stream: false\n'
            'routing:\n  mode: auto\n'
            f'  max_turn_usd: {budget}\n  output_tokens: 256\n'
            '  candidates:\n'
            '  - {model: route-fast, quality: 1}\n'
            '  - {model: route-deep, quality: 3, effort: medium}\n')
        subprocess.run([binary, 'init'], cwd=project, check=True,
                       capture_output=True, timeout=30)
        root_url = f'http://127.0.0.1:{port}' + ('/wrong' if endpoint_mismatch else '')
        (project / 'catalog.yaml').write_text(
            'models:\n'
            '- name: route-fast\n  modalities: [text]\n'
            '  capabilities: [tool_calling]\n'
            '- name: route-deep\n  modalities: [text]\n'
            '  capabilities: [tool_calling, reasoning, reasoning_effort]\n'
            'providers:\n'
            '- name: route-provider\n'
            f'  root_url: {root_url}\n'
            '  endpoints:\n  - protocol: chat_completions\n'
            '    endpoint: /v1/chat/completions\n'
            '  models:\n'
            '  - canonical_id: route-fast\n    provider_model_id: fast-wire\n'
            '    pricing: {currency: USD, unit: per_million_tokens, input: 1, output: 2}\n'
            '  - canonical_id: route-deep\n    provider_model_id: deep-wire\n'
            '    pricing: {currency: USD, unit: per_million_tokens, input: 5, output: 10}\n')
        subprocess.run([binary, 'catalog', 'import', 'catalog.yaml'], cwd=project,
                       check=True, capture_output=True, timeout=30)
        if endpoint_mismatch:
            subprocess.run([binary, 'config', 'set', 'api_url',
                            f'http://127.0.0.1:{port}/v1/chat/completions'],
                           cwd=project, check=True, capture_output=True,
                           timeout=30)
        peer = subprocess.Popen([binary, 'acp'], cwd=project,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, bufsize=0,
                                env={**os.environ, 'FYAI_TRACE': str(project / 'trace.log')})
        buffer = bytearray()
        frames = []

        def send(request):
            peer.stdin.write((json.dumps({'jsonrpc': '2.0', **request}) + '\n').encode())
            peer.stdin.flush()

        def until_id(wanted):
            nonlocal buffer
            limit = time.monotonic() + 30
            while time.monotonic() < limit:
                if b'\n' in buffer:
                    line, _, rest = buffer.partition(b'\n')
                    buffer = bytearray(rest)
                    frame = json.loads(line)
                    frames.append(frame)
                    if frame.get('id') == wanted:
                        return frame
                    continue
                if peer.poll() is not None:
                    raise RuntimeError(f'engine exited: {peer.stderr.read()!r}')
                ready, _, _ = select.select([peer.stdout], [], [],
                                            limit - time.monotonic())
                if ready:
                    buffer.extend(os.read(peer.stdout.fileno(), 65536))
            raise TimeoutError(frames)

        send({'id': 1, 'method': 'initialize',
              'params': {'protocolVersion': 1,
                         'clientCapabilities': {'session': {'notices': {}}}}})
        assert until_id(1).get('result', {}).get('protocolVersion') == 1
        send({'id': 2, 'method': 'session/new',
              'params': {'cwd': str(project), 'mcpServers': []}})
        created = until_id(2)
        session = created['result']['sessionId']
        options = created['result']['configOptions']
        model = next(option for option in options if option['id'] == 'model')
        assert model['currentValue'] == 'auto', model
        send({'id': 3, 'method': 'session/prompt',
              'params': {'sessionId': session,
                         'prompt': [{'type': 'text',
                                     'text': 'Debug this failing build carefully.'}]}})
        completed = until_id(3)
        requests = project / 'requests.jsonl'
        if should_succeed:
            assert completed.get('result', {}).get('stopReason') == 'end_turn', (
                completed, frames, (project / 'trace.log').read_text()
                if (project / 'trace.log').exists() else '')
            assert requests.exists(), frames
            body = json.loads(requests.read_text().splitlines()[0])['body']
            assert body['model'] == 'deep-wire', body
            assert body['reasoning_effort'] == 'medium', body
            notices = [f['params']['update'] for f in frames
                       if f.get('method') == 'session/update' and
                       f.get('params', {}).get('update', {}).get('sessionUpdate') == 'notice']
            assert any('catalogue estimate' in n.get('description', '') and
                       'deep-wire' in n.get('description', '') for n in notices), notices
            if missing_usage:
                assert any(n.get('title') == 'Task cost' and
                           'Cost unavailable' in n.get('description', '')
                           for n in notices), notices
            else:
                assert any(n.get('title') == 'Task cost' and
                           '$0.000450 (catalogue estimate)' in n.get('description', '')
                           for n in notices), notices
            send({'id': 4, 'method': 'session/set_config_option',
                  'params': {'sessionId': session, 'configId': 'model',
                             'value': 'route-fast'}})
            changed = until_id(4)
            assert changed.get('result'), changed
            send({'id': 5, 'method': 'session/prompt',
                  'params': {'sessionId': session,
                             'prompt': [{'type': 'text',
                                         'text': 'Debug this failing build carefully.'}]}})
            manual = until_id(5)
            assert manual.get('result', {}).get('stopReason') == 'end_turn', manual
            send({'id': 6, 'method': 'session/set_config_option',
                  'params': {'sessionId': session, 'configId': 'model',
                             'value': 'auto'}})
            changed = until_id(6)
            assert changed.get('result'), changed
            send({'id': 7, 'method': 'session/prompt',
                  'params': {'sessionId': session,
                             'prompt': [{'type': 'text', 'text': 'hi'}]}})
            second = until_id(7)
            assert second.get('result', {}).get('stopReason') == 'end_turn', second
            bodies = [json.loads(line)['body'] for line in requests.read_text().splitlines()]
            assert len(bodies) == 3, bodies
            assert bodies[1]['model'] == 'fast-wire', bodies[1]
            assert 'reasoning_effort' not in bodies[1], bodies[1]
            assert bodies[2]['model'] == 'fast-wire', bodies[2]
            assert 'reasoning_effort' not in bodies[2], bodies[2]
            if manual_override:
                peer.terminate()
                peer.communicate(timeout=5)
                peer = None
                result = subprocess.run(
                    [binary, '-m', 'route-fast', 'Debug this failing build.'],
                    cwd=project, capture_output=True, timeout=30)
                assert result.returncode == 0, result.stderr
                bodies = [json.loads(line)['body'] for line in requests.read_text().splitlines()]
                assert len(bodies) == 4, bodies
                assert bodies[3]['model'] == 'fast-wire', bodies[3]
                assert 'reasoning_effort' not in bodies[3], bodies[3]
        else:
            assert completed.get('error'), completed
            assert not requests.exists(), requests.read_text() if requests.exists() else ''
    finally:
        if peer is not None:
            peer.terminate()
            try:
                peer.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                peer.kill()
                peer.communicate()
        server.terminate()
        try:
            server.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.communicate()


exercise(0.05, True, manual_override=True)
exercise(1, True)
exercise(0.05, True, missing_usage=True)
exercise(0.0000001, False)
exercise(0.05, False, endpoint_mismatch=True)
print('ACP routing, effort, notice, and pre-request budget rejection passed.')
