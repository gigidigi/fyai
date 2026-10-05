#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify ACP tool events, questions, writer exclusion and child cleanup.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
"$PYTHON" "$TESTS_DIR/acp_m4_probe.py" "$FYAI_BIN" || \
    fail "ACP M4 contract failed"
pass
