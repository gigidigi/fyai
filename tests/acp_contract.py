import json
import os
import select
import subprocess
import sys
import tempfile
import time
from pathlib import Path


root = Path(__file__).resolve().parent.parent if len(sys.argv) > 1 else Path("/home/giorgos/fyai-acp")
binary = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build/fyai"
fixtures = Path.cwd() if len(sys.argv) > 1 else Path("/home/giorgos/.fyai-desktop/fixtures")
fixtures.mkdir(parents=True, exist_ok=True)
timeout_scale = max(1, int(os.environ.get("FYAI_TIMEOUT_SCALE", "1")))
project = Path(tempfile.mkdtemp(prefix="m3-acp-", dir=fixtures))
server = subprocess.Popen(["python3", str(root / "tests/mock/mock_provider.py"),
                           str(root / "tests/scenarios/chat_basic.json"), str(project)],
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def wait_port(project, server):
    for _ in range(100 * timeout_scale):
        if (project / "port").exists():
            return (project / "port").read_text().strip()
        time.sleep(0.1)
    raise RuntimeError(f"mock provider failed: {server.stderr.read()!r}")


def send(proc, method, request_id, params, split=False):
    payload = {"jsonrpc": "2.0", "method": method, "params": params}
    if request_id is not None:
        payload["id"] = request_id
    frame = (json.dumps(payload, ensure_ascii=False) + "\n").encode()
    if split:
        point = frame.index(b"\xce") + 1 if b"\xce" in frame else 29
        for chunk in [frame[:7], frame[7:point], frame[point:]]:
            proc.stdin.write(chunk)
            proc.stdin.flush()
    else:
        proc.stdin.write(frame)
        proc.stdin.flush()


def response(proc, request_id, timeout=30):
    end = time.monotonic() + timeout * timeout_scale
    frames = []
    buffer = bytearray()
    while time.monotonic() < end:
        remaining = end - time.monotonic()
        ready, _, _ = select.select([proc.stdout], [], [], min(remaining, 0.2))
        if not ready:
            if proc.poll() is not None:
                raise RuntimeError(f"engine exited {proc.returncode}; stderr={proc.stderr.read()!r}")
            continue
        chunk = os.read(proc.stdout.fileno(), 65536)
        if not chunk:
            raise RuntimeError(f"engine closed stdout; stderr={proc.stderr.read()!r}")
        buffer.extend(chunk)
        while b"\n" in buffer:
            line, _, rest = buffer.partition(b"\n")
            buffer = bytearray(rest)
            frame = json.loads(line.decode("utf-8"))
            frames.append(frame)
            if frame.get("id") == request_id:
                return frame, frames
    raise TimeoutError(f"no response to {request_id}; frames={frames!r}")


try:
    port = wait_port(project, server)
    (project / "config.yaml").write_text(
        "api: chat-completions\n"
        f"api_url: http://127.0.0.1:{port}/v1/chat/completions\n"
        "auth: api-key\nno_auth: true\n"
        "display:\n  stream: false\n"
    )
    init = subprocess.run([str(binary), "init"], cwd=project,
                          capture_output=True, text=True, timeout=15)
    if init.returncode:
        raise RuntimeError(f"init failed: {init.stderr}")
    (project / "catalog.yaml").write_text(
        "models:\n"
        "- name: acp-local-mock\n"
        "  capabilities: [reasoning, reasoning_effort]\n"
        "providers:\n"
        "- name: acp-local-provider\n"
        f"  root_url: http://127.0.0.1:{port}\n"
        "  endpoints:\n"
        "  - protocol: chat_completions\n"
        "    endpoint: /v1/chat/completions\n"
        "  models:\n"
        "  - canonical_id: acp-local-mock\n"
        "    provider_model_id: mock-model\n"
    )
    imported = subprocess.run([str(binary), "catalog", "import", "catalog.yaml"],
                              cwd=project, capture_output=True, text=True, timeout=15)
    if imported.returncode:
        raise RuntimeError(f"local catalogue import failed: {imported.stderr}")
    proc = subprocess.Popen([str(binary), "acp"], cwd=project,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, bufsize=0)
    try:
        send(proc, "initialize", 1, {"protocolVersion": 1,
             "clientCapabilities": {}, "clientInfo": {"name": "m3-probe", "version": "1"}}, split=True)
        initialized, _ = response(proc, 1)
        assert initialized["result"]["protocolVersion"] == 1, initialized
        send(proc, "session/new", 2, {"cwd": str(project), "mcpServers": []})
        created, _ = response(proc, 2)
        session = created["result"]["sessionId"]
        options = created["result"]["configOptions"]
        model = next(o for o in options if o["id"] == "model")
        assert model["options"], options
        send(proc, "session/set_config_option", 20,
             {"sessionId": session, "configId": "model", "value": "not-in-catalog"})
        rejected, _ = response(proc, 20)
        assert rejected["error"]["code"] == -32602, rejected
        send(proc, "session/set_config_option", 21,
             {"sessionId": session, "configId": "model", "value": "acp-local-mock"})
        selected, _ = response(proc, 21)
        assert "result" in selected, selected
        selected_options = selected["result"]["configOptions"]
        assert next(o for o in selected_options if o["id"] == "model")["currentValue"] == "acp-local-mock", selected
        effort = next(o for o in selected_options if o["id"] == "effort")
        assert any(o["value"] == "high" for o in effort["options"]), selected_options
        send(proc, "session/set_config_option", 22,
             {"sessionId": session, "configId": "effort", "value": "high"})
        selected, _ = response(proc, 22)
        assert "result" in selected, selected
        assert next(o for o in selected["result"]["configOptions"]
                    if o["id"] == "effort")["currentValue"] == "high", selected
        send(proc, "session/prompt", 3,
             {"sessionId": session, "prompt": [{"type": "text", "text": "γειά from ACP"}]}, split=True)
        prompted, frames = response(proc, 3)
        assert "result" in prompted, prompted
        assert prompted["result"]["stopReason"] == "end_turn", prompted
        live_text = "".join(f["params"]["update"]["content"]["text"] for f in frames
                            if f.get("method") == "session/update" and
                            f["params"]["update"]["sessionUpdate"] == "agent_message_chunk")
        assert live_text == "Hello from the mock provider.", frames
        requests = [json.loads(line) for line in (project / "requests.jsonl").read_text().splitlines()]
        assert len(requests) == 1, requests
        assert requests[0]["path"] == "/v1/chat/completions", requests
        assert requests[0]["body"]["model"] == "mock-model", requests
        assert requests[0]["body"]["reasoning_effort"] == "high", requests
        expected_transcript = [("user_message_chunk", "γειά from ACP"),
                               ("agent_message_chunk", live_text)]
        print("ACP local model/effort selection and prompt passed; session", session)
    finally:
        proc.stdin.close()
        try:
            proc.wait(timeout=10 * timeout_scale)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        if proc.returncode:
            print("ACP stderr:", proc.stderr.read().decode(errors="replace"))
    reopened = subprocess.Popen([str(binary), "acp"], cwd=project,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, bufsize=0)
    try:
        send(reopened, "initialize", 4,
             {"protocolVersion": 1, "clientCapabilities": {}})
        response(reopened, 4)
        send(reopened, "session/list", 5, {"cwd": str(project)})
        listed, _ = response(reopened, 5)
        assert any(item["sessionId"] == session for item in listed["result"]["sessions"]), listed
        send(reopened, "session/load", 6,
             {"sessionId": session, "cwd": str(project), "mcpServers": []})
        loaded, replay = response(reopened, 6)
        assert "result" in loaded, loaded
        replay_updates = [f["params"]["update"] for f in replay
                          if f.get("method") == "session/update"]
        replay_transcript = [(u["sessionUpdate"], u["content"]["text"])
                             for u in replay_updates]
        assert replay_transcript == expected_transcript, (replay_transcript, expected_transcript)
        replay_ids = [u.get("messageId") for u in replay_updates]
        assert all(replay_ids) and len(set(replay_ids)) == len(replay_ids), replay_updates
        send(reopened, "session/close", 7, {"sessionId": session})
        closed, _ = response(reopened, 7)
        assert closed["result"] == {}, closed
        print("ACP list, exact load replay with message IDs and close passed")
    finally:
        reopened.stdin.close()
        try:
            reopened.wait(timeout=10 * timeout_scale)
        except subprocess.TimeoutExpired:
            reopened.kill()
            reopened.wait(timeout=5)
        if reopened.returncode:
            print("ACP reopen stderr:", reopened.stderr.read().decode(errors="replace"))
    resumed = subprocess.Popen([str(binary), "acp"], cwd=project,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, bufsize=0)
    try:
        send(resumed, "initialize", 15, {"protocolVersion": 1, "clientCapabilities": {}})
        response(resumed, 15)
        send(resumed, "session/resume", 16,
             {"sessionId": session, "cwd": str(project), "mcpServers": []})
        attached, frames = response(resumed, 16)
        assert "result" in attached, attached
        assert not any(f.get("method") == "session/update" for f in frames), frames
        send(resumed, "session/close", 17, {"sessionId": session})
        response(resumed, 17)
        print("ACP resume attached without replay")
    finally:
        resumed.stdin.close()
        resumed.wait(timeout=10 * timeout_scale)
finally:
    server.terminate()
    server.wait(timeout=5)

cancel_project = Path(tempfile.mkdtemp(prefix="m3-cancel-", dir=fixtures))
cancel_server = subprocess.Popen(["python3", str(root / "tests/mock/mock_provider.py"),
                                  str(root / "tests/scenarios/interrupt.json"), str(cancel_project)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    cancel_port = wait_port(cancel_project, cancel_server)
    (cancel_project / "config.yaml").write_text(
        "api: chat-completions\n"
        f"api_url: http://127.0.0.1:{cancel_port}/v1/chat/completions\n"
        "auth: api-key\nno_auth: true\n"
    )
    init = subprocess.run([str(binary), "init"], cwd=cancel_project,
                          capture_output=True, text=True, timeout=15)
    if init.returncode:
        raise RuntimeError(f"cancel fixture init failed: {init.stderr}")
    cancelling = subprocess.Popen([str(binary), "acp"], cwd=cancel_project,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, bufsize=0)
    try:
        send(cancelling, "initialize", 8,
             {"protocolVersion": 1, "clientCapabilities": {}})
        response(cancelling, 8)
        send(cancelling, "session/new", 9,
             {"cwd": str(cancel_project), "mcpServers": []})
        created, _ = response(cancelling, 9)
        cancel_session = created["result"]["sessionId"]
        send(cancelling, "session/prompt", 10,
             {"sessionId": cancel_session, "prompt": [{"type": "text", "text": "cancel now"}]})
        send(cancelling, "session/cancel", None, {"sessionId": cancel_session})
        stopped, frames = response(cancelling, 10, timeout=10)
        assert stopped["result"]["stopReason"] == "cancelled", (stopped, frames)
        send(cancelling, "session/prompt", 11,
             {"sessionId": cancel_session, "prompt": [{"type": "text", "text": "cancel during request"}]})
        time.sleep(0.5)
        send(cancelling, "session/cancel", None, {"sessionId": cancel_session})
        stopped, frames = response(cancelling, 11, timeout=10)
        assert stopped["result"]["stopReason"] == "cancelled", (stopped, frames)
        assert not any("This reply should never be delivered." in json.dumps(f) for f in frames), frames
        print("ACP queued and in-flight cancellation passed")
    finally:
        cancelling.stdin.close()
        try:
            cancelling.wait(timeout=10 * timeout_scale)
        except subprocess.TimeoutExpired:
            cancelling.kill()
            cancelling.wait(timeout=5)
        if cancelling.returncode:
            print("ACP cancel stderr:", cancelling.stderr.read().decode(errors="replace"))
    after_cancel = subprocess.Popen([str(binary), "acp"], cwd=cancel_project,
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, bufsize=0)
    try:
        send(after_cancel, "initialize", 21,
             {"protocolVersion": 1, "clientCapabilities": {}})
        response(after_cancel, 21)
        send(after_cancel, "session/list", 22, {"cwd": str(cancel_project)})
        listed, _ = response(after_cancel, 22)
        assert any(s["sessionId"] == cancel_session for s in listed["result"]["sessions"]), listed
        send(after_cancel, "session/load", 23,
             {"sessionId": cancel_session, "cwd": str(cancel_project), "mcpServers": []})
        loaded, replay = response(after_cancel, 23)
        assert "result" in loaded, loaded
        assert not any(f.get("method") == "session/update" for f in replay), replay
        send(after_cancel, "session/close", 24, {"sessionId": cancel_session})
        response(after_cancel, 24)
        print("Empty cancelled session persisted without a submitted prompt")
    finally:
        after_cancel.stdin.close()
        after_cancel.wait(timeout=10 * timeout_scale)
finally:
    cancel_server.terminate()
    cancel_server.wait(timeout=5)

failed_project = Path(tempfile.mkdtemp(prefix="m3-publish-", dir=fixtures))
failed_server = subprocess.Popen(["python3", str(root / "tests/mock/mock_provider.py"),
                                  str(root / "tests/scenarios/chat_basic.json"), str(failed_project)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    failed_port = wait_port(failed_project, failed_server)
    (failed_project / "config.yaml").write_text(
        "api: chat-completions\n"
        f"api_url: http://127.0.0.1:{failed_port}/v1/chat/completions\n"
        "auth: api-key\nno_auth: true\n"
    )
    init = subprocess.run([str(binary), "init"], cwd=failed_project,
                          capture_output=True, text=True, timeout=15)
    if init.returncode:
        raise RuntimeError(f"publish fixture init failed: {init.stderr}")
    failed_env = dict(os.environ, FYAI_TEST_PUBLISH_FAIL_AT="2")
    failing = subprocess.Popen([str(binary), "acp"], cwd=failed_project,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, env=failed_env, bufsize=0)
    try:
        send(failing, "initialize", 12,
             {"protocolVersion": 1, "clientCapabilities": {}})
        response(failing, 12)
        send(failing, "session/new", 13,
             {"cwd": str(failed_project), "mcpServers": []})
        created, _ = response(failing, 13)
        failed_session = created["result"]["sessionId"]
        send(failing, "session/prompt", 14,
             {"sessionId": failed_session, "prompt": [{"type": "text", "text": "must fail publication"}]})
        failed, _ = response(failing, 14)
        assert failed["error"]["code"] == -32000, failed
        assert "not published" in failed["error"]["message"], failed
        print("ACP publication failure returned a protocol error")
    finally:
        failing.stdin.close()
        try:
            failing.wait(timeout=10 * timeout_scale)
        except subprocess.TimeoutExpired:
            failing.kill()
            failing.wait(timeout=5)
    recheck = subprocess.Popen([str(binary), "acp"], cwd=failed_project,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, bufsize=0)
    try:
        send(recheck, "initialize", 18, {"protocolVersion": 1, "clientCapabilities": {}})
        response(recheck, 18)
        send(recheck, "session/load", 19,
             {"sessionId": failed_session, "cwd": str(failed_project), "mcpServers": []})
        loaded, replay = response(recheck, 19)
        assert "result" in loaded, loaded
        assert not any("must fail publication" in json.dumps(f) or
                       "Hello from the mock provider." in json.dumps(f) for f in replay), replay
        send(recheck, "session/close", 20, {"sessionId": failed_session})
        response(recheck, 20)
        print("Failed publication left no saved user or assistant message")
    finally:
        recheck.stdin.close()
        recheck.wait(timeout=10 * timeout_scale)
finally:
    failed_server.terminate()
    failed_server.wait(timeout=5)
