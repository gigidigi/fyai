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
project = Path(tempfile.mkdtemp(prefix='m4-probe-', dir=fixtures))
scenario = project / 'scenario.json'


def response(message, reason='stop'):
    return {'id': 'mock', 'object': 'chat.completion', 'created': 1735689600,
            'model': 'mock-model', 'choices': [{'index': 0, 'message': message,
                                               'finish_reason': reason}]}


def make_server(tool, final='done'):
    scenario.write_text(json.dumps({'steps': [
        {'response': response({'role': 'assistant', 'content': None,
                               'tool_calls': [tool]}, 'tool_calls')},
        {'response': response({'role': 'assistant', 'content': final})}]}))
    server = subprocess.Popen(['python3', str(root / 'tests/mock/mock_provider.py'),
                               str(scenario), str(project)], stderr=subprocess.PIPE)
    deadline = time.monotonic() + 20
    while not (project / 'port').exists():
        if time.monotonic() > deadline:
            raise TimeoutError('mock port')
        if server.poll() is not None:
            raise RuntimeError(server.stderr.read())
    port = (project / 'port').read_text().strip()
    (project / 'config.yaml').write_text('api: chat-completions\n'
        f'api_url: http://127.0.0.1:{port}/v1/chat/completions\n'
        'auth: api-key\nno_auth: true\ndisplay:\n  stream: false\n')
    subprocess.run([binary, 'init'], cwd=project, check=True, capture_output=True)
    return server


class Peer:
    def __init__(self):
        self.proc = subprocess.Popen([binary, 'acp'], cwd=project,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.buffer = bytearray()
        self.frames = []

    def send(self, method, id, params):
        frame = {'jsonrpc': '2.0', 'method': method, 'params': params}
        if id is not None:
            frame['id'] = id
        self.proc.stdin.write((json.dumps(frame) + '\n').encode())
        self.proc.stdin.flush()

    def reply(self, id, result):
        self.proc.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': id,
                                           'result': result}) + '\n').encode())
        self.proc.stdin.flush()

    def next(self, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if b'\n' in self.buffer:
                line, _, rest = self.buffer.partition(b'\n')
                self.buffer = bytearray(rest)
                try:
                    frame = json.loads(line)
                except json.JSONDecodeError as error:
                    raise RuntimeError(f'bad ACP frame: {line!r}') from error
                self.frames.append(frame)
                if frame.get('method') == 'session/request_permission':
                    self.reply(frame['id'], {'outcome': {'outcome': 'selected', 'optionId': 'allow-once'}})
                    continue
                return frame
            if self.proc.poll() is not None:
                raise RuntimeError(f'engine exited: {self.proc.stderr.read()!r}')
            ready, _, _ = select.select([self.proc.stdout], [], [], deadline-time.monotonic())
            if ready:
                self.buffer.extend(os.read(self.proc.stdout.fileno(), 65536))
        raise TimeoutError(self.frames)

    def until(self, id):
        while True:
            frame = self.next()
            if frame.get('id') == id:
                return frame

    def init(self, form=False):
        capabilities = {'elicitation': {'form': {}}} if form else {}
        self.send('initialize', 1, {'protocolVersion': 1,
                                    'clientCapabilities': capabilities})
        initialized = self.until(1)['result']
        assert initialized['_meta']['_fyai/pid'] > 0, initialized
        assert initialized['_meta']['_fyai/pgid'] == initialized['_meta']['_fyai/pid'], initialized
        self.send('session/new', 2, {'cwd': str(project), 'mcpServers': []})
        return self.until(2)['result']['sessionId']

    def stop(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=15)
        if self.proc.returncode:
            print('stderr:', self.proc.stderr.read().decode(errors='replace'))


tool = {'id': 'call_write_1', 'type': 'function', 'function': {
    'name': 'write_file', 'arguments': json.dumps({'path': 'hello.txt', 'content': 'new\n'})}}
server = make_server(tool)
try:
    (project / 'hello.txt').write_text('old\n')
    p = Peer()
    sid = p.init()
    # A concurrent CLI or ACP writer must get a clear busy result.
    busy = subprocess.run([binary, 'acp'], cwd=project, input=b'', capture_output=True, timeout=10)
    assert busy.returncode != 0 and b'busy in another process' in busy.stderr, busy
    cli_busy = subprocess.run([binary, 'config', 'get', 'model'], cwd=project,
                              capture_output=True, timeout=10)
    assert cli_busy.returncode != 0 and b'busy in another process' in cli_busy.stderr, cli_busy
    p.send('session/prompt', 3, {'sessionId': sid, 'prompt': [{'type':'text','text':'edit'}]})
    completed = p.until(3)
    assert completed.get('result', {}).get('stopReason') == 'end_turn', completed
    updates = [f['params']['update'] for f in p.frames if f.get('method') == 'session/update']
    calls = [u for u in updates if u['sessionUpdate'] in ('tool_call', 'tool_call_update')]
    assert [u['status'] for u in calls] == ['pending','in_progress','completed'], calls
    assert all(u['toolCallId'] == 'call_write_1' for u in calls), calls
    diff = [b for b in calls[-1]['content'] if b['type'] == 'diff']
    assert len(diff) == 1 and diff[0]['oldText'] == 'old\n' and diff[0]['newText'] == 'new\n', calls
    assert diff[0]['path'] == str(project / 'hello.txt'), diff
    assert (project / 'hello.txt').read_text() == 'new\n'
    print('writer lock, tool lifecycle and write_file diff passed')
    p.stop()
    live = [(u['sessionUpdate'], u.get('content'), u.get('toolCallId'),
             u.get('status'), u.get('name'), u.get('kind'),
             u.get('locations')) for u in updates]
    q = Peer()
    q.send('initialize', 11, {'protocolVersion': 1, 'clientCapabilities': {}})
    q.until(11)
    q.send('session/load', 12, {'sessionId': sid, 'cwd': str(project),
                                'mcpServers': []})
    loaded = q.until(12)
    assert 'result' in loaded, loaded
    replay = [f['params']['update'] for f in q.frames if f.get('method') == 'session/update'
              and f['params']['update']['sessionUpdate'] != 'user_message_chunk']
    replay_shape = [(u['sessionUpdate'], u.get('content'), u.get('toolCallId'),
                     u.get('status'), u.get('name'), u.get('kind'),
                     u.get('locations')) for u in replay]
    assert replay_shape == live, (replay_shape, live)
    replay_calls = [u for u in replay if u['sessionUpdate'] in ('tool_call','tool_call_update')]
    replay_diff = [b for b in replay_calls[-1]['content'] if b['type'] == 'diff']
    assert replay_diff == diff, (replay_diff, diff)
    q.stop()
    print('exact tool and text replay passed')
finally:
    server.terminate()
    server.wait(timeout=5)


project = Path(tempfile.mkdtemp(prefix='m4-output-', dir=fixtures))
tool = {'id': 'call_read_long', 'type': 'function', 'function': {
    'name': 'read_file', 'arguments': json.dumps({'path': 'long.txt'})}}
server = make_server(tool)
try:
    (project / 'long.txt').write_text('λ' * 10000)
    p = Peer()
    sid = p.init()
    p.send('session/prompt', 3, {'sessionId': sid,
                                'prompt': [{'type': 'text', 'text': 'read long'}]})
    assert p.until(3).get('result', {}).get('stopReason') == 'end_turn'
    calls = [f['params']['update'] for f in p.frames if f.get('method') == 'session/update'
             and f['params']['update']['sessionUpdate'] == 'tool_call_update']
    text = calls[-1]['content'][0]['content']['text']
    assert 4000 <= len(text.encode('utf-8')) <= 4096, len(text.encode('utf-8'))
    p.stop()
    print('bounded UTF-8 tool output passed')
finally:
    server.terminate()
    server.wait(timeout=5)


project = Path(tempfile.mkdtemp(prefix='m4-unified-', dir=fixtures))
patch = '--- a/hello.c\n+++ b/hello.c\n@@ -1 +1 @@\n-old\n+new\n'
tool = {'id': 'call_unified_1', 'type': 'function', 'function': {
    'name': 'apply_patch', 'arguments': json.dumps({'patch': patch})}}
server = make_server(tool)
try:
    (project / 'hello.c').write_text('old\n')
    p = Peer()
    sid = p.init()
    p.send('session/prompt', 3, {'sessionId': sid,
                                'prompt': [{'type': 'text', 'text': 'unified patch'}]})
    assert p.until(3).get('result', {}).get('stopReason') == 'end_turn'
    calls = [f['params']['update'] for f in p.frames if f.get('method') == 'session/update'
             and f['params']['update']['sessionUpdate'] == 'tool_call_update']
    diff = [b for b in calls[-1]['content'] if b['type'] == 'diff']
    assert len(diff) == 1 and diff[0]['oldText'] == 'old\n' and diff[0]['newText'] == 'new\n', diff
    p.stop()
    print('unified apply_patch diff passed')
finally:
    server.terminate()
    server.wait(timeout=5)


project = Path(tempfile.mkdtemp(prefix='m4-patch-', dir=fixtures))
patch = ('*** Begin Patch\n*** Update File: hello.c\n@@\n-old\n+new\n'
         '*** Add File: added.txt\n+first\n*** End Patch\n')
tool = {'id': 'call_patch_1', 'type': 'function', 'function': {
    'name': 'apply_patch', 'arguments': json.dumps({'patch': patch})}}
server = make_server(tool)
try:
    (project / 'hello.c').write_text('old\n')
    p = Peer()
    sid = p.init()
    p.send('session/prompt', 3, {'sessionId': sid,
                                'prompt': [{'type': 'text', 'text': 'patch'}]})
    done = p.until(3)
    assert done.get('result', {}).get('stopReason') == 'end_turn', done
    calls = [f['params']['update'] for f in p.frames if f.get('method') == 'session/update'
             and f['params']['update']['sessionUpdate'] == 'tool_call_update']
    edits = [b for b in calls[-1]['content'] if b['type'] == 'diff']
    assert len(edits) == 2, edits
    by_path = {e['path']: e for e in edits}
    assert by_path[str(project / 'hello.c')]['oldText'] == 'old\n', edits
    assert by_path[str(project / 'hello.c')]['newText'] == 'new\n', edits
    assert 'oldText' not in by_path[str(project / 'added.txt')], edits
    assert by_path[str(project / 'added.txt')]['newText'] == 'first\n', edits
    p.stop()
    q = Peer()
    q.send('initialize', 11, {'protocolVersion': 1, 'clientCapabilities': {}})
    q.until(11)
    q.send('session/load', 12, {'sessionId': sid, 'cwd': str(project),
                                'mcpServers': []})
    q.until(12)
    replay_calls = [f['params']['update'] for f in q.frames if f.get('method') == 'session/update'
                    and f['params']['update']['sessionUpdate'] == 'tool_call_update']
    replay_edits = [b for b in replay_calls[-1]['content'] if b['type'] == 'diff']
    assert replay_edits == edits, (replay_edits, edits)
    q.stop()
    print('multi-file apply_patch diffs and replay passed')
finally:
    server.terminate()
    server.wait(timeout=5)


for action in ('accept', 'decline', 'cancel', 'disconnect', 'unsupported'):
    project = Path(tempfile.mkdtemp(prefix='m4-question-', dir=fixtures))
    tool = {'id': 'call_ask_1', 'type': 'function', 'function': {
        'name': 'ask_user', 'arguments': json.dumps({
            'question': 'Proceed with the mock plan?', 'options': ['yes', 'no']})}}
    server = make_server(tool, 'User said yes, proceeding.')
    try:
        p = Peer()
        sid = p.init(form=action != 'unsupported')
        p.send('session/prompt', 3, {'sessionId': sid,
                                    'prompt': [{'type': 'text', 'text': 'question'}]})
        if action != 'unsupported':
            while True:
                frame = p.next()
                if frame.get('method') == 'elicitation/create':
                    break
            params = frame['params']
            assert params['sessionId'] == sid and params['toolCallId'] == 'call_ask_1', frame
            assert params['mode'] == 'form', frame
            answer_field = params['requestedSchema']['properties']['answer']
            assert 'yes' in answer_field['description'] and 'no' in answer_field['description'], frame
            if action == 'disconnect':
                p.stop()
                print('question connection loss passed')
                continue
            reply = {'action': action}
            if action == 'accept':
                reply['content'] = {'answer': 'yes'}
            p.reply(frame['id'], reply)
        result = p.until(3)
        if action == 'accept':
            assert result.get('result', {}).get('stopReason') == 'end_turn', result
        else:
            assert 'result' in result or 'error' in result, result
            requests = [json.loads(line) for line in (project / 'requests.jsonl').read_text().splitlines()]
            assert len(requests) == 1, requests
        p.stop()
        print('question', action, 'passed')
    finally:
        server.terminate()
        server.wait(timeout=5)


project = Path(tempfile.mkdtemp(prefix='m4-child-question-', dir=fixtures))
server = subprocess.Popen(['python3', str(root / 'tests/mock/mock_provider.py'),
    str(root / 'tests/scenarios/agent_asks_user.json'), str(project)],
    stderr=subprocess.PIPE)
try:
    deadline = time.monotonic() + 20
    while not (project / 'port').exists():
        assert time.monotonic() < deadline, 'mock port'
    port = (project / 'port').read_text().strip()
    (project / 'config.yaml').write_text('api: responses\n'
        f'api_url: http://127.0.0.1:{port}/v1/responses\n'
        'auth: api-key\nno_auth: true\ndisplay:\n  stream: false\n')
    subprocess.run([binary, 'init'], cwd=project, check=True, capture_output=True)
    p = Peer()
    sid = p.init(form=True)
    p.send('session/prompt', 3, {'sessionId': sid,
                                'prompt': [{'type': 'text', 'text': 'ask a sub-agent'}]})
    while True:
        frame = p.next()
        if frame.get('method') == 'elicitation/create':
            break
        if frame.get('id') == 3:
            p.stop()
            print('delegated stderr:', p.proc.stderr.read().decode(errors='replace'))
            raise AssertionError(frame)
    assert "sub-agent 'asker'" in frame['params']['message'], frame
    assert frame['params']['toolCallId'] == 'c1', frame
    p.reply(frame['id'], {'action': 'accept', 'content': {'answer': 'green'}})
    result = p.until(3)
    assert result.get('result', {}).get('stopReason') == 'end_turn', result
    requests = [json.loads(line)['body'] for line in
                (project / 'requests.jsonl').read_text().splitlines()]
    child = [r for r in requests if 'fyai sub-agent' in json.dumps(r)]
    assert len(child) >= 2 and 'green' in json.dumps(child[-1]['input']), child
    p.stop()
    print('delegated form question and answer passed')
finally:
    server.terminate()
    server.wait(timeout=5)


for ending in ('cancel', 'disconnect'):
    project = Path(tempfile.mkdtemp(prefix='m4-shell-', dir=fixtures))
    command = 'echo $$ > shell.pid; sleep 60'
    tool = {'id': 'call_shell_1', 'type': 'function', 'function': {
        'name': 'shell', 'arguments': json.dumps({'command': command})}}
    server = make_server(tool)
    try:
        p = Peer()
        sid = p.init()
        p.send('session/prompt', 3, {'sessionId': sid,
                                    'prompt': [{'type': 'text', 'text': 'long tool'}]})
        deadline = time.monotonic() + 20
        while not (project / 'shell.pid').exists():
            assert time.monotonic() < deadline, 'shell never started'
            try:
                frame = p.next(timeout=0.2)
                assert frame.get('id') != 3, frame
            except TimeoutError:
                pass
        shell_pid = int((project / 'shell.pid').read_text())
        if ending == 'cancel':
            p.send('session/cancel', None, {'sessionId': sid})
            result = p.until(3)
            assert result.get('result', {}).get('stopReason') == 'cancelled', result
            p.stop()
        else:
            p.stop()
            requests_before = (project / 'requests.jsonl').read_text().splitlines()
            q = Peer()
            q.send('initialize', 11, {'protocolVersion': 1,
                                      'clientCapabilities': {}})
            q.until(11)
            q.send('session/load', 12, {'sessionId': sid, 'cwd': str(project),
                                        'mcpServers': []})
            loaded = q.until(12)
            assert 'result' in loaded, loaded
            q.stop()
            assert (project / 'requests.jsonl').read_text().splitlines() == requests_before
        try:
            os.killpg(shell_pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError(f'shell process group {shell_pid} survived {ending}')
        print('long-tool', ending, 'group cleanup passed')
    finally:
        server.terminate()
        server.wait(timeout=5)
