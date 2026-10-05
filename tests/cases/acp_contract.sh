#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify ACP v1 session transport against local mock providers.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
"$PYTHON" "$TESTS_DIR/acp_contract.py" "$FYAI_BIN" || \
    fail "ACP v1 contract failed"
pass
