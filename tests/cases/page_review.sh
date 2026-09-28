#!/bin/bash
# SPDX-License-Identifier: MIT
# fyai page review draws a sample page with each area painted and named, and
# lists the areas; a machine format gives only the list.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
out=$("$FYAI_BIN" --color=off page review question --width 70 --height 22) ||
    fail "page review did not draw the question sample"
for name in transcript "pane/blank" pane.cap "header.shown/blank" header \
        question ask.from_agent "ask.options/selected" "ask.options/other" \
        prompt status.hint status.row; do
    printf '%s\n' "$out" | grep -q -F " $name " ||
        fail "page review did not name the area $name"
done
# The row of the question keeps its text; the name stands at the right.
printf '%s\n' "$out" | grep -q -E '^  \? Apply the patch\?.* question $' ||
    fail "the name of a row covered its text"

json=$("$FYAI_BIN" page review popup --width 60 --height 12 --output json) ||
    fail "page review --output json failed"
"$PYTHON" - "$json" <<'PY' || fail "the areas of the popup are wrong"
import json
import sys

areas = json.loads(sys.argv[1])
names = [a["area"] for a in areas]
if "popup" not in names:
    raise SystemExit("no popup slot in %r" % names)
popup = next(a for a in areas if a["area"] == "popup" and a["kind"] == "slot")
if popup["kind"] != "slot" or popup["row"] != 1 or popup["height"] != 11:
    raise SystemExit("popup slot %r" % popup)
if "\x1b" in sys.argv[1] or "transcript" in names:
    raise SystemExit("the picture or a covered area reached the list")
PY
pass
