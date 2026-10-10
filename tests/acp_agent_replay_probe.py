# SPDX-License-Identifier: MIT
import json
import os
import select
import subprocess
import sys
import tempfile
import time
from pathlib import Path

binary = str(Path(sys.argv[1]).resolve())
base = Path.cwd()
with tempfile.TemporaryDirectory(prefix='agent-replay-', dir=base) as scratch:
    project = Path(scratch)
    env = os.environ.copy()
    for name in ('HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        env[name] = str(project / name.lower())
        Path(env[name]).mkdir()
    (project / 'config.yaml').write_text('api: chat-completions\napi_url: http://127.0.0.1:1/v1/chat/completions\nauth: api-key\nno_auth: true\n')
    document = ''.join('<!-- meta:yaml\n' + item + '\n-->\n' for item in (
        'kind: conversation\nformat: 2', 'kind: publish', 'kind: turn',
        'kind: message\nrole: agent')) + 'AGENT REPLAY MARKER\n'
    saved = project / 'conversation.md'
    saved.write_text(document)
    for args in (['init'], ['import', '-i', str(saved), '--ignore-compact']):
        subprocess.run([binary, *args], cwd=project, env=env, check=True,
                       capture_output=True, timeout=20)
    with subprocess.Popen([binary, 'acp'], cwd=project, env=env,
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, bufsize=0) as peer:
        frames = []
        buffer = bytearray()

        def request(number, method, params):
            global buffer
            peer.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': number,
                                         'method': method, 'params': params}) + '\n').encode())
            peer.stdin.flush()
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if b'\n' in buffer:
                    line, _, rest = buffer.partition(b'\n')
                    buffer = bytearray(rest)
                    frame = json.loads(line)
                    frames.append(frame)
                    if frame.get('id') == number:
                        assert 'result' in frame, frame
                        return frame['result']
                else:
                    ready, _, _ = select.select([peer.stdout], [], [], deadline-time.monotonic())
                    if ready:
                        data = os.read(peer.stdout.fileno(), 65536)
                        assert data, peer.stderr.read()
                        buffer.extend(data)
            raise TimeoutError(frames)

        try:
            request(1, 'initialize', {'protocolVersion': 1, 'clientCapabilities': {}})
            request(2, 'session/load', {'sessionId': 'main', 'cwd': str(project), 'mcpServers': []})
            updates = [f['params']['update'] for f in frames if f.get('method') == 'session/update']
            text = [u['content']['text'] for u in updates if u['sessionUpdate'] == 'agent_message_chunk']
            assert text == ['**Agent ?** (message ?)\n\nAGENT REPLAY MARKER'], updates
            print('Canonical agent-role text survived ACP replay.')
        finally:
            peer.stdin.close()
            try:
                peer.wait(timeout=5)
            except subprocess.TimeoutExpired:
                peer.terminate()
                peer.wait(timeout=5)
