#!/bin/bash
# SPDX-License-Identifier: MIT
# The catalogue of a branch is edited as the configuration is: get, set and
# delete by path, checked against the catalogue schema, and reset to the
# embedded catalogue. `catalog update` takes what catalog_update/command writes.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# A path names an item of a sequence by its name.
run_fyai catalog get models/claude-opus-5/name
assert_status 0
assert_stdout_contains "claude-opus-5"

run_fyai catalog set models/claude-opus-5/context_window 1234
assert_status 0
run_fyai catalog get models/claude-opus-5/context_window
assert_status 0
assert_stdout_contains "1234"

# The schema refuses a value of the wrong type, and nothing is written.
run_fyai catalog set models/claude-opus-5/context_window '"many"'
assert_status 1
assert_stderr_contains "context_window: type mismatch"
run_fyai catalog get models/claude-opus-5/context_window
assert_stdout_contains "1234"

# A set on an item that does not exist adds it, named by the path.
run_fyai catalog set models/edit-model '{capabilities: []}'
assert_status 0
run_fyai catalog get models/edit-model/name
assert_stdout_contains "edit-model"
run_fyai catalog delete models/edit-model
assert_status 0
run_fyai catalog get models/edit-model
assert_status 1
assert_stderr_contains "nothing at 'models/edit-model'"

# The change is of this branch alone.
run_fyai branch create other main
assert_status 0
run_fyai catalog reset
assert_status 0
run_fyai catalog get models/claude-opus-5/context_window
assert_stdout_not_contains "1234"
run_fyai -b other catalog get models/claude-opus-5/context_window
assert_stdout_contains "1234"

run_fyai catalog validate
assert_status 0
assert_stdout_contains "catalog: valid"

# --- update ---------------------------------------------------------------
cat > scraper.sh <<'EOS'
#!/bin/sh
echo "$*" > "${0%/*}/scraper.args"
[ -z "$OPENAI_API_KEY" ] || echo leaked > "${0%/*}/scraper.leak"
[ -z "$DEEPSEEK_API_KEY" ] || echo "$DEEPSEEK_API_KEY" > "${0%/*}/scraper.key"
cat <<EOY
models:
- name: scraped-model
  context_window: 7
providers:
- name: scrapedprov
  root_url: http://127.0.0.1:1
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: scraped-model
    provider_model_id: scraped-model
EOY
EOS
chmod +x scraper.sh
run_fyai config set catalog_update/command "\"$TEST_DIR/scraper.sh\""
assert_status 0

# A selection of providers is merged: the rest of the catalogue stays.
export OPENAI_API_KEY=not-for-the-scraper
run_fyai catalog update --provider scrapedprov --curated
assert_status 0
unset OPENAI_API_KEY
test ! -e scraper.leak || fail "the scraper was given a provider key"
grep -qx -- "--format yaml --curated --provider scrapedprov" scraper.args || \
	fail "unexpected scraper arguments: $(cat scraper.args)"
run_fyai catalog get models/scraped-model/context_window
assert_stdout_contains "7"
run_fyai catalog get models/claude-opus-5/name
assert_status 0

# A credential that catalog_update/credentials names reaches the command;
# the others do not.
run_fyai config set catalog_update/credentials '[DEEPSEEK_API_KEY]'
assert_status 0
export OPENAI_API_KEY=not-for-the-scraper DEEPSEEK_API_KEY=for-the-scraper
run_fyai catalog update --provider scrapedprov
assert_status 0
unset OPENAI_API_KEY DEEPSEEK_API_KEY
test ! -e scraper.leak || fail "the scraper was given a key it did not name"
grep -qx for-the-scraper scraper.key || \
	fail "the scraper was not given the key that it names"

# Without a selection the output is the catalogue.
run_fyai catalog update
assert_status 0
assert_stdout_contains "1 models, 1 providers"

# A failed run says why and changes nothing.
# fyai appends its options; sh -c takes them as its own arguments, as exit
# does not take them on every shell.
run_fyai config set catalog_update/command "\"sh -c 'echo broken >&2; exit 3'\""
assert_status 0
run_fyai catalog update
assert_status 1
assert_stderr_contains "exited with status 3: broken"
run_fyai catalog get models/scraped-model/name
assert_status 0

pass
