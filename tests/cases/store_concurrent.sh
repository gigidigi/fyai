#!/bin/bash
# SPDX-License-Identifier: MIT
# Two commands that change the store of one branch at the same time. The one
# that loses the race merges its store three ways against the one that won:
# changes to different keys are kept, and a key that both set to different
# values is a conflict that branch/on_conflict decides, as for turns.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# Hold both publishers at a gate after they have opened the same root, so
# that one of them loses the race.
run_race() {
	tag=$1
	shift
	gate="$TEST_DIR/gate-$tag"
	mkdir -p "$gate"
	set +e
	FYAI_TEST_BRANCH_CAS_GATE="$gate" FYAI_TEST_BRANCH_CAS_PEERS=2 \
		"$FYAI_BIN" --color off "$@" \
		>"$TEST_DIR/$tag.left.out" 2>"$TEST_DIR/$tag.left.err" &
	left_pid=$!
	FYAI_TEST_BRANCH_CAS_GATE="$gate" FYAI_TEST_BRANCH_CAS_PEERS=2 \
		"$FYAI_BIN" --color off "${RIGHT[@]}" \
		>"$TEST_DIR/$tag.right.out" 2>"$TEST_DIR/$tag.right.err" &
	right_pid=$!
	wait "$left_pid"
	left_status=$?
	wait "$right_pid"
	right_status=$?
	set -e
}

# Different keys of the configuration: both are kept.
RIGHT=(config set temperature 0.5)
run_race keys config set top_p 0.25
[ "$left_status" -eq 0 ] && [ "$right_status" -eq 0 ] || \
	fail "disjoint configuration changes failed: $(cat "$TEST_DIR"/keys.*.err)"
run_fyai config get temperature
assert_stdout_contains "0.5"
run_fyai config get top_p
assert_stdout_contains "0.25"

# Different models of the catalogue: both are kept.
RIGHT=(catalog set models/claude-sonnet-5/context_window 222)
run_race models catalog set models/claude-opus-5/context_window 111
[ "$left_status" -eq 0 ] && [ "$right_status" -eq 0 ] || \
	fail "disjoint catalogue changes failed: $(cat "$TEST_DIR"/models.*.err)"
run_fyai catalog get models/claude-opus-5/context_window
assert_stdout_contains "111"
run_fyai catalog get models/claude-sonnet-5/context_window
assert_stdout_contains "222"

# The same key: the loser writes nothing and says what conflicts.
RIGHT=(config set temperature 0.75)
run_race same config set temperature 0.25
[ $((left_status + right_status)) -eq 1 ] || \
	fail "one of two conflicting changes must fail ($left_status, $right_status)"
grep -qF "a concurrent change also set config/temperature; nothing was written" \
	"$TEST_DIR"/same.*.err || fail "the conflict was not reported"
if [ "$left_status" -eq 0 ]; then want=0.25; else want=0.75; fi
run_fyai config get temperature
assert_stdout_contains "$want"

# branch/on_conflict rebase keeps the value of the loser, and says so.
run_fyai config set branch/on_conflict rebase
assert_status 0
RIGHT=(config set temperature 0.9)
run_race rebase config set temperature 0.1
[ "$left_status" -eq 0 ] && [ "$right_status" -eq 0 ] || \
	fail "rebase did not resolve the conflict: $(cat "$TEST_DIR"/rebase.*.err)"
grep -qF "kept the values of this command" "$TEST_DIR"/rebase.* || \
	fail "the kept conflict was not reported"

pass
