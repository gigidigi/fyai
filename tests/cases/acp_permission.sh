#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that ACP permission denial stops tools before execution.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
"$PYTHON" "$TESTS_DIR/acp_permission_probe.py" "$FYAI_BIN" ||
    fail "ACP permission contract failed"
pass
