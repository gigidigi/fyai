#!/bin/bash
# SPDX-License-Identifier: MIT
# -h and --help are the help command: the words after them are its topic. An
# option fyai does not know names the help command.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup_bare

check_same()
{
	run_fyai help "$@"
	assert_status 0
	cp "$TEST_DIR/stdout" "$TEST_DIR/help.out"
	run_fyai -h "$@"
	assert_status 0
	cmp -s "$TEST_DIR/help.out" "$TEST_DIR/stdout" ||
		fail "fyai -h $* is not fyai help $*"
	run_fyai --help "$@"
	assert_status 0
	cmp -s "$TEST_DIR/help.out" "$TEST_DIR/stdout" ||
		fail "fyai --help $* is not fyai help $*"
}

check_same
check_same branch
check_same config set
check_same refs

run_fyai -h
assert_stdout_contains "Global options"
assert_stdout_contains "-h, --help"

run_fyai --no-such-option
assert_status_nonzero
assert_stderr_contains "fyai help"

pass
