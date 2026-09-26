#!/bin/bash
# SPDX-License-Identifier: MIT
# /catalog update runs catalog_update/command in a tile of the work pane, with
# the credentials that catalog_update/credentials names, and commits the
# catalogue that it writes to the branch of the session.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

cat > scraper.sh <<'EOS'
#!/bin/sh
echo "$*" > "${0%/*}/scraper.args"
[ -z "$OPENAI_API_KEY" ] || echo leaked > "${0%/*}/scraper.leak"
[ -z "$DEEPSEEK_API_KEY" ] || echo "$DEEPSEEK_API_KEY" > "${0%/*}/scraper.key"
printf '%s%s\n' SCRAPER- PROGRESS >&2
cat <<EOY
models:
- name: tile-model
  context_window: 7
providers:
- name: tileprov
  root_url: http://127.0.0.1:1
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: tile-model
    provider_model_id: tile-model
EOY
EOS
chmod +x scraper.sh
run_fyai config set catalog_update/command "\"$TEST_DIR/scraper.sh\""
assert_status 0
run_fyai config set catalog_update/credentials '[DEEPSEEK_API_KEY]'
assert_status 0

# The progress of the program is on its tile, and the result follows its end.
# The session then reads the catalogue that it committed.
OPENAI_API_KEY=not-for-the-scraper DEEPSEEK_API_KEY=for-the-scraper \
FYAI_PTY_INPUT="/catalog update --provider tileprov" \
FYAI_PTY_NEEDLE="catalog-1" \
FYAI_PTY_AFTER="wait-screen:SCRAPER-PROGRESS|wait-screen:catalog: updated|raw:1d|send:/catalog get models/tile-model|wait-screen:context_window: 7" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true -m mock-model -i

test ! -e scraper.leak || fail "the scraper was given a key it did not name"
grep -qx for-the-scraper scraper.key || \
	fail "the scraper was not given the key that it names"
grep -qx -- "--format yaml --provider tileprov" scraper.args || \
	fail "unexpected scraper arguments: $(cat scraper.args)"

pass
