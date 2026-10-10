#!/bin/bash
# SPDX-License-Identifier: MIT
# The configuration limits the time that a delegation can request.
#
# The model supplies the call `timeout`, so agent/max_timeout_ms bounds it.
# This rule is equivalent to shell/max_timeout_ms for the shell tool. The
# maximum does not apply to operator-configured persona and default limits.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
# Scale both deadlines so the child can start under a sanitizer.
requested_ms=$((1500 * FYAI_TIMEOUT_SCALE))
bounded_ms=$((800 * FYAI_TIMEOUT_SCALE))
"$PYTHON" - "$SCENARIOS_DIR/agent_call_timeout.json" "$requested_ms" \
	> agent_call_timeout_scaled.json <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    scenario = json.load(source)
call = scenario["steps"][0]["response"]["output"][0]
args = json.loads(call["arguments"])
args["timeout"] = int(sys.argv[2])
call["arguments"] = json.dumps(args)
json.dump(scenario, sys.stdout)
PY
mock_start agent_call_timeout_scaled.json

# The call requests 1500 units, and the configured maximum is 800 units.
run_fyai --set display/stream=false --set agent/timeout_ms=0 \
	 --set agent/max_timeout_ms="$bounded_ms" --set agent/hang_timeout_ms="$requested_ms" \
	 --set api=responses --set api_url="$MOCK_URL/v1/responses" \
	 -m mock-model "delegate a task that does not end"
assert_status 0
assert_stdout_contains "The sub-agent was stopped."
assert_any_request "'agent hang timeout after $((bounded_ms + requested_ms)) ms' in json.dumps(r)"
assert_any_request "any(item.get('content') == 'x' for item in r['body'].get('input', []))"

mock_stop 3
pass
