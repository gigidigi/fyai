#!/bin/bash
# SPDX-License-Identifier: MIT
# A verb that sends no model request needs no provider key; a verb that
# does reports the missing key.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# Before the loop: api below stores a grammar that ChatGPT does not take.
set +e
${FYAI_VALGRIND} "$FYAI_BIN" --color off compact \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
status=$?
set -e
[ "$status" -ne 0 ] || fail "compact ran without a key"
grep -qi "no API key" "$TEST_DIR/stderr" ||
	fail "compact did not report the missing key: $(cat "$TEST_DIR/stderr")"

for verb in "branch" "config show" "catalog show" "history" "stats" \
	    "list models" "diff main main" "model" "help branch" \
	    "model gpt-5.4-mini" "api" "api chat-completions" "root show"; do
	# shellcheck disable=SC2086
	${FYAI_VALGRIND} "$FYAI_BIN" --color off $verb \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null ||
		fail "'$verb' failed without a key: $(cat "$TEST_DIR/stderr")"
	grep -qi "api key" "$TEST_DIR/stderr" &&
		fail "'$verb' asked for an API key"
done

pass
