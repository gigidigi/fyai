#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
"$PYTHON" "$TESTS_DIR/acp_route_probe.py" "$FYAI_BIN" ||
    fail "ACP automatic routing contract failed"
pass
