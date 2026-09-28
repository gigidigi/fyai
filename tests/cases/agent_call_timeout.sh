#!/bin/bash
# SPDX-License-Identifier: MIT
# A delegation requests a time limit.
#
# The call limit has priority over the persona limit and agent/timeout_ms.
# The model supplies the call limit, so agent/max_timeout_ms bounds it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_call_timeout.json

# agent/timeout_ms disables the default limit. The call supplies the advisory
# limit, and the hang limit ends this job.
run_fyai --set display/stream=false --set agent/timeout_ms=0 \
	 --set agent/hang_timeout_ms=1500 \
	 --set api=responses --set api_url="$MOCK_URL/v1/responses" \
	 -m mock-model "delegate a task that does not end"
assert_status 0
assert_stdout_contains "The sub-agent was stopped."
assert_any_request "'agent hang timeout after 3000 ms' in json.dumps(r)"

mock_stop 3
pass
