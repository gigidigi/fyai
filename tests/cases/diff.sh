#!/bin/bash
# SPDX-License-Identifier: MIT
# diff compares the exports of two ref-log entries. A bare diff shows the last
# change of the active branch.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

run_fyai branch describe main "the diff branch"
assert_status 0
run_fyai branch create described
assert_status 0
run_fyai config set temperature 0.5
assert_status 0

# The last change is the configuration.
run_fyai diff
assert_status 0
assert_stdout_contains "--- HEAD@{1}"
assert_stdout_contains "+++ HEAD"
assert_stdout_contains "+  config/temperature: 0.5"
assert_stdout_not_contains "the diff branch"

# Two entries: the description is the change between them.
run_fyai diff 'main@{2}' 'main@{1}'
assert_status 0
assert_stdout_contains "the diff branch"
assert_stdout_not_contains "temperature"

run_fyai diff main HEAD
assert_status 0
assert_stdout_contains "diff: main and HEAD are the same"

# A turn reference names no ref-log entry.
run_fyai diff 'main~1'
assert_status 1
assert_stderr_contains "does not name a ref-log entry"
run_fyai diff 'main@{99}'
assert_status 1
assert_stderr_contains "ref log has no entry 99"

# -u is the unified text, as a pipe gets it anyway.
run_fyai diff -u
assert_status 0
assert_stdout_contains "+  config/temperature: 0.5"

# --root reads both references in that root: a later change does not move it.
run_fyai root print
assert_status 0
root=$(cat "$TEST_DIR/stdout")
run_fyai diff
cp "$TEST_DIR/stdout" pinned.diff
run_fyai branch create before-quarter
assert_status 0
run_fyai config set temperature 0.25
assert_status 0
run_fyai --root "$root" diff
assert_status 0
cmp -s pinned.diff "$TEST_DIR/stdout" || fail "diff under --root moved"
run_fyai diff 'main@{1}' 'main'
assert_stdout_contains "+  config/temperature: 0.25"

# A terminal gets coloured rows on its own background with -u, as without
# Markdown; a pipe gets the text, as the cases above read it. The
# diff view of a palette theme washes a changed row with a colour mixed with
# the background that the terminal reports, and draws no ground of its own.
cat > "$TEST_DIR/diff_pty.py" <<'PY'
import os, pty, re, select, sys, time
from term_reply import answer_da1

BIN, DIR, MODE = sys.argv[1], sys.argv[2], sys.argv[3]
BACKGROUND = sys.argv[4].encode() if len(sys.argv) > 4 else None
args = [BIN, "-k", "test-key", "--color", "on"]
if MODE == "view":
    args += ["--theme", "ember:dark", "--set", "display/markdown=true"]
# A --set publishes: name both entries, not the last change.
args += ["diff", "before-quarter", "main"]
if MODE == "unified":
    args.append("-u")
pid, fd = pty.fork()
if pid == 0:
    os.chdir(DIR)
    os.environ["TERM"] = "xterm-256color"
    # the wash is compared as a truecolor value; a runner states no depth
    os.environ["COLORTERM"] = "truecolor"
    os.execv(BIN, args)
scale = float(os.environ.get("FYAI_TIMEOUT_SCALE", "1"))
buf, end = b"", time.time() + 20 * scale
while time.time() < end:
    if select.select([fd], [], [], 0.2)[0]:
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        if BACKGROUND and b"\x1b]11;?" in chunk:
            os.write(fd, b"\x1b]11;" + BACKGROUND + b"\x1b\\")
        answer_da1(fd, chunk)
        buf += chunk
try:
    os.close(fd)
except OSError:
    pass
os.waitpid(pid, 0)
rows = buf.split(b"\n")
plain = lambda l: re.sub(rb"\x1b\[[0-9;]*m", b"", l)
row = [l for l in rows if b"config/temperature: 0.25" in plain(l)]
if len(row) != 1:
    sys.exit("no single temperature row: %r" % buf[-400:])
row = row[0]
if MODE in ("color", "unified"):
    if b"\x1b[" not in row or b"[48;" in row:
        sys.exit("color row: %r" % row)
    sys.exit(0)
# view: the added row has a wash, a context row has no background.
wash = re.search(rb"\x1b\[48;2;(\d+;\d+;\d+)m\+", row)
if not wash:
    sys.exit("view row has no wash: %r" % row)
BAR = "\u2502".encode()
context = [l for l in rows if BAR in plain(l) and
           plain(l).split(BAR, 1)[1].startswith(b"  ")]
if not context or any(b"[48;" in l for l in context):
    sys.exit("a context row has a background: %r" % context[:1])
print(wash.group(1).decode())
PY
for mode in color unified; do
	"$PYTHON" "$TEST_DIR/diff_pty.py" "$FYAI_BIN" "$TEST_DIR" "$mode" || \
		fail "diff on a terminal ($mode)"
done
wash_a=$("$PYTHON" "$TEST_DIR/diff_pty.py" "$FYAI_BIN" "$TEST_DIR" view \
	rgb:1010/1010/1010) || fail "diff view: $wash_a"
wash_b=$("$PYTHON" "$TEST_DIR/diff_pty.py" "$FYAI_BIN" "$TEST_DIR" view \
	rgb:2828/2020/1818) || fail "diff view: $wash_b"
[ "$wash_a" != "$wash_b" ] || \
	fail "the wash did not follow the terminal background ($wash_a)"

# /diff runs the same comparison in a session. The session publishes when it
# starts, so it names a branch and not a count of entries.
set +e
"$FYAI_BIN" -k test-key -b main --color off --set display/markdown=false -i \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" <<'EOS'
/diff described main
/diff main~1
EOS
set -e
grep -q -- '--- described' "$TEST_DIR/stdout" || fail "/diff wrote no diff"
grep -q '+  config/temperature: 0.5' "$TEST_DIR/stdout" || \
	fail "/diff did not show the configuration change"
grep -q 'does not name a ref-log entry' "$TEST_DIR/stdout" \
	"$TEST_DIR/stderr" || fail "/diff accepted a turn reference"

pass
