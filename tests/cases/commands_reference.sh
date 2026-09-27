#!/bin/bash
# SPDX-License-Identifier: MIT
# doc/commands.md is generated from the command definitions: it must match.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup_bare

run_fyai help --markdown
assert_status 0
diff -u "$TESTS_DIR/../doc/commands.md" "$TEST_DIR/stdout" >"$TEST_DIR/diff" ||
	fail "doc/commands.md is out of date; run ninja docs-commands"

run_fyai help --man
assert_status 0
assert_stdout_contains ".TH FYAI 1"
assert_stdout_contains ".SS \"fyai branch new NAME [REF]\""

pass
