#!/bin/sh
# Smoke test for the built viper-daemon binary: real process, real signals.
# Usage: sh tests/daemon_smoke.sh [path-to-viper-daemon]
set -e

DAEMON="${1:-./build-host/viper-daemon}"
ROOT="$(mktemp -d)"
SOCK="viper4android.smoke.$$"
STATUS=0

fail() {
    echo "FAILED: $1" >&2
    STATUS=1
}

echo "daemon=$DAEMON"
echo "state_root=$ROOT"
echo "socket=$SOCK"

"$DAEMON" --state-root "$ROOT" --socket "$SOCK" --poll-ms 10 &
DPID=$!
sleep 1

echo "--- state file ---"
if [ -f "$ROOT/daemon.state" ]; then
    cat "$ROOT/daemon.state"
else
    fail "daemon.state was not written"
fi

echo "--- SIGTERM shutdown ---"
kill -TERM "$DPID" 2>/dev/null || fail "daemon was not running"
wait "$DPID" 2>/dev/null
echo "wait_exit=$?"
if kill -0 "$DPID" 2>/dev/null; then
    fail "daemon survived SIGTERM"
    kill -KILL "$DPID" 2>/dev/null || true
fi

echo "--- leftover files ---"
ls -1 "$ROOT"
if [ -e "$ROOT/daemon.state.tmp" ]; then
    fail "temporary state file was left behind"
fi

echo "--- second instance must refuse the busy socket ---"
"$DAEMON" --state-root "$ROOT" --socket "$SOCK" --poll-ms 10 &
HOLDER=$!
sleep 1
if "$DAEMON" --state-root "$ROOT" --socket "$SOCK" --status >/dev/null 2>&1; then
    fail "second daemon bound an already-used socket"
fi
kill -TERM "$HOLDER" 2>/dev/null || true
wait "$HOLDER" 2>/dev/null || true

echo "--- status mode ---"
"$DAEMON" --state-root "$ROOT" --socket "${SOCK}.status" --status || fail "--status failed"

echo "--- argument validation ---"
if "$DAEMON" --poll-ms 0 >/dev/null 2>&1; then
    fail "--poll-ms 0 was accepted"
fi
if "$DAEMON" --bogus-flag >/dev/null 2>&1; then
    fail "unknown flag was accepted"
fi

rm -rf "$ROOT"
if [ "$STATUS" -eq 0 ]; then
    echo "daemon smoke test passed"
else
    echo "daemon smoke test FAILED"
fi
exit "$STATUS"
