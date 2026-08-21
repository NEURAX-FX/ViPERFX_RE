#!/system/bin/sh
# Checks that init owns the daemon: killing it must produce a new process under
# pid 1, and its sockets must come back.
#
# This is the reboot-gated half of the init-import verification. It does not
# reboot: it only exercises what can be observed on a running system.
#
# The daemon pid is resolved from /proc by matching the executable name and
# requiring PPID 1. A `pgrep -f` pattern would also match the shell running this
# script, whose own cmdline contains the pattern.
#
# Usage (as root, on the device):
#   sh tests/module/probe_init_restart.sh
set -u

SERVICE=viper_daemon
STATE=/data/adb/viper4android
STATUS=0

fail() {
    echo "FAILED: $1" >&2
    STATUS=1
}

# Only init's own child counts: a scratch daemon from an acceptance script would
# have a different parent and must never be picked up here.
daemon_pid() {
    for entry in /proc/[0-9]*; do
        pid=${entry#/proc/}
        [ -r "$entry/comm" ] || continue
        [ "$(cat "$entry/comm" 2>/dev/null)" = "viper-daemon" ] || continue
        ppid=$(awk '/^PPid:/ { print $2 }' "$entry/status" 2>/dev/null)
        [ "$ppid" = "1" ] || continue
        printf '%s\n' "$pid"
        return 0
    done
    return 1
}

# Listening endpoints only. /proc/net/unix lists live client connections under the
# same abstract name, so a raw name count also moves when a peer connects or
# disconnects, which says nothing about whether the daemon rebound its sockets.
# Field 6 is the socket state: 01 is unconnected/listening, 03 is connected.
sockets() {
    awk '$6 == "01" && $8 ~ /^@viper4android\.(driver|app)\.v1$/ { count++ }
         END { print count + 0 }' /proc/net/unix
}

[ "$(id -u)" = "0" ] || { echo 'FAILED: must run as root' >&2; exit 1; }

INIT_BEFORE=$(getprop "init.svc.$SERVICE" 2>/dev/null || true)
PID_BEFORE=$(daemon_pid || true)
SOCKETS_BEFORE=$(sockets)
echo "init_svc_before=${INIT_BEFORE:-unset} pid_before=${PID_BEFORE:-none} sockets_before=$SOCKETS_BEFORE"

[ "$INIT_BEFORE" = "running" ] || { fail "init does not report $SERVICE running"; exit "$STATUS"; }
[ -n "${PID_BEFORE:-}" ] || { fail 'no init-owned daemon process'; exit "$STATUS"; }
[ "$SOCKETS_BEFORE" -ge 2 ] || fail "expected driver and app sockets, found $SOCKETS_BEFORE"

# SIGKILL: a crash gives init no chance to be told, which is the case under test.
kill -9 "$PID_BEFORE"

# Poll instead of sleeping a fixed interval: init's restart delay is not a
# published constant.
i=0
while [ "$i" -lt 30 ]; do
    PID_AFTER=$(daemon_pid || true)
    if [ -n "${PID_AFTER:-}" ] && [ "${PID_AFTER:-}" != "$PID_BEFORE" ]; then
        break
    fi
    i=$((i + 1))
    sleep 1
done
PID_AFTER=$(daemon_pid || true)
INIT_AFTER=$(getprop "init.svc.$SERVICE" 2>/dev/null || true)

# The socket is rebound by the new process, so it can lag the pid slightly.
i=0
while [ "$i" -lt 20 ]; do
    SOCKETS_AFTER=$(sockets)
    [ "$SOCKETS_AFTER" -ge "$SOCKETS_BEFORE" ] && break
    i=$((i + 1))
    sleep 1
done
SOCKETS_AFTER=$(sockets)
echo "init_svc_after=${INIT_AFTER:-unset} pid_after=${PID_AFTER:-none} sockets_after=$SOCKETS_AFTER"

[ -n "${PID_AFTER:-}" ] || fail 'init did not restart the daemon'
[ "${PID_AFTER:-}" != "$PID_BEFORE" ] || fail 'daemon pid unchanged; the kill did not take effect'
[ "$INIT_AFTER" = "running" ] || fail "init reports $SERVICE as ${INIT_AFTER:-unset} after restart"
[ "$SOCKETS_AFTER" -ge "$SOCKETS_BEFORE" ] \
    || fail "sockets did not come back ($SOCKETS_BEFORE -> $SOCKETS_AFTER)"

# A restarted daemon that cannot serve is not a successful restart.
grep -qE '^app_listening=1' "$STATE/daemon.state" 2>/dev/null \
    || fail 'restarted daemon did not bind the App endpoint'
echo "state: $(grep -E '^(driver_connected|app_listening|route_known|daemon_generation)=' "$STATE/daemon.state" 2>/dev/null | tr '\n' ' ')"

[ "$STATUS" -eq 0 ] && echo 'INIT_RESTART_PROBE=PASS'
exit "$STATUS"
