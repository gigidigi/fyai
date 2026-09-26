#!/bin/bash
# SPDX-License-Identifier: MIT
# Each branch keeps its rarely changed state in its store: the configuration,
# the catalogue and the description. A new branch takes the store of its start
# point, a catalogue import changes the current branch alone, and an export
# carries the store to another arena.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

cat > catalog.yaml <<EOF2
models:
- name: store-model
  capabilities: []
providers:
- name: storeprov
  root_url: http://127.0.0.1:1
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: store-model
    provider_model_id: store-model
EOF2

run_fyai branch create before
assert_status 0

run_fyai catalog import catalog.yaml
assert_status 0

# The import is a change of main; a branch made before it keeps its own.
run_fyai catalog list models
assert_status 0
assert_stdout_contains "store-model"
run_fyai -b before catalog list models
assert_status 0
assert_stdout_not_contains "store-model"

# A new branch takes the store of its start point.
run_fyai branch create after
assert_status 0
run_fyai -b after catalog list models
assert_status 0
assert_stdout_contains "store-model"

run_fyai branch describe main "the store branch"
assert_status 0

run_fyai export -o saved.md
assert_status 0
grep -qx 'kind: store' saved.md || fail "the store was not exported"
grep -q 'catalog/models:' saved.md || fail "the catalogue was not exported"

# A second arena takes the description and the catalogue with the turns.
export HOME="$TEST_DIR/home2"
mkdir -p "$HOME" "$TEST_DIR/second"
cd "$TEST_DIR/second"
run_fyai init
assert_status 0
run_fyai import -i ../saved.md
assert_status 0
run_fyai catalog list models
assert_status 0
assert_stdout_contains "store-model"
run_fyai branch show main
assert_status 0
assert_stdout_contains "the store branch"

# A catalogue reset is carried as the removal of the member: the second arena
# returns to the embedded catalogue, and its root stays valid.
cd "$TEST_DIR"
export HOME="$TEST_DIR/home"
run_fyai catalog reset
assert_status 0
run_fyai export -o reset.md
assert_status 0
grep -qx '  catalog: null' reset.md || \
	fail "the reset was not exported as the removal of the catalogue"
export HOME="$TEST_DIR/home3"
mkdir -p "$HOME" "$TEST_DIR/third"
cd "$TEST_DIR/third"
run_fyai init
assert_status 0
run_fyai import -i ../reset.md
assert_status 0
run_fyai catalog list models
assert_status 0
assert_stdout_not_contains "store-model"
assert_stdout_contains "claude-opus-5"

pass
