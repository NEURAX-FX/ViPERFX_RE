#!/bin/sh
# Exercises every branch of module/daemon-start.sh with stubbed getprop/start,
# since a real init service cannot be registered without installing the module.
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d)"
STATUS=0

fail() {
    echo "FAILED: $1" >&2
    STATUS=1
}

# Builds an isolated copy of the helper whose STATE_DIR and PATH are local, so
# the stubs below shadow the real getprop/start.
setup_case() {
    CASE="$WORK/$1"
    rm -rf "$CASE"
    mkdir -p "$CASE/bin" "$CASE/stub" "$CASE/state"
    sed "s|^STATE_DIR=.*|STATE_DIR=$CASE/state|" "$REPO_ROOT/module/daemon-start.sh" \
        > "$CASE/daemon-start.sh"
    printf '#!/bin/sh\nprintf "%%s" "%s"\n' "$2" > "$CASE/stub/getprop"
    printf '#!/bin/sh\necho "$@" >> %s/start-calls\n' "$CASE" > "$CASE/stub/start"
    chmod +x "$CASE/stub/getprop" "$CASE/stub/start"
    : > "$CASE/start-calls"
    if [ "$3" = "with-binary" ]; then
        printf '#!/bin/sh\nexit 0\n' > "$CASE/bin/viper-daemon"
        chmod +x "$CASE/bin/viper-daemon"
    fi
}

run_case() {
    ( PATH="$CASE/stub:$PATH" sh "$CASE/daemon-start.sh" )
    echo "$?"
}

marker() { cat "$CASE/state/daemon-start.log" 2>/dev/null; }
start_calls() { wc -l < "$CASE/start-calls"; }

echo "--- service absent: report unsupported, do not start ---"
setup_case absent "" with-binary
RC="$(run_case)"
[ "$RC" = "0" ] || fail "absent-service exit was $RC, expected 0"
marker | grep -q "unsupported" || fail "absent-service marker not written"
[ "$(start_calls)" = "0" ] || fail "start was called without an init service"

echo "--- service stopped: ask init to start it ---"
setup_case stopped "stopped" with-binary
RC="$(run_case)"
[ "$RC" = "0" ] || fail "stopped-service exit was $RC, expected 0"
grep -q "viper_daemon" "$CASE/start-calls" || fail "init was not asked to start the service"
[ "$(start_calls)" = "1" ] || fail "expected exactly one start request"
marker | grep -q "requested start" || fail "start was not recorded"

echo "--- service running: idempotent no-op ---"
setup_case running "running" with-binary
RC="$(run_case)"
[ "$RC" = "0" ] || fail "running-service exit was $RC, expected 0"
[ "$(start_calls)" = "0" ] || fail "start was called for an already-running service"
marker | grep -q "already running" || fail "running state not recorded"

echo "--- repeated invocation never starts twice ---"
setup_case repeat "running" with-binary
run_case >/dev/null
run_case >/dev/null
run_case >/dev/null
[ "$(start_calls)" = "0" ] || fail "repeated runs issued start requests"
[ "$(marker | wc -l)" = "3" ] || fail "expected one marker line per run"

echo "--- binary missing: fail loudly, do not start ---"
setup_case nobinary "stopped"
RC="$(run_case)"
[ "$RC" = "1" ] || fail "missing-binary exit was $RC, expected 1"
marker | grep -q "error" || fail "missing binary not reported"
[ "$(start_calls)" = "0" ] || fail "start was called without a daemon binary"

echo "--- state dir is root-private ---"
setup_case perms "running" with-binary
run_case >/dev/null
MODE="$(stat -c %a "$CASE/state")"
[ "$MODE" = "700" ] || fail "state dir mode is $MODE, expected 700"

rm -rf "$WORK"
if [ "$STATUS" -eq 0 ]; then
    echo "daemon-start helper smoke test passed"
else
    echo "daemon-start helper smoke test FAILED"
fi
exit "$STATUS"
