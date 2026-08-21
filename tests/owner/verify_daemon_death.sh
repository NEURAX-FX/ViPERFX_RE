#!/system/bin/sh
# Acceptance check: killing the daemon must not stop audio processing.
#
# The daemon is the control plane, not the effect client. If the owner releases
# its handle when the daemon's socket closes, then every daemon restart - update,
# crash, init respawn - silences audio, which is the failure the owner exists to
# prevent.
#
# Runs an isolated daemon on unique abstract socket names and its own state root,
# so the installed init-supervised daemon is untouched.
#
# Usage (as root, on the device):
#   sh tests/owner/verify_daemon_death.sh
set -u

REPO_DIR="${REPO_DIR:-/data/data/com.tom.rv2ide/files/home/ViPERFX_RE}"
BIN_DIR="${BIN_DIR:-$REPO_DIR/out/magisk_module/common/bin}"
WORK=/data/local/tmp/viper-daemon-death-check
VIPER_UUID=90380da3-8536-4744-a6a3-5731970e640f
STATUS=0
FIRST_DAEMON=
SECOND_DAEMON=
OWNER_PID=
TAG=$$

fail() {
    echo "FAILED: $1" >&2
    STATUS=1
}

modules() {
    dumpsys media.audio_flinger 2>/dev/null | grep -c "$VIPER_UUID"
}

state_value() {
    sed -n "s/^$1=//p" "$WORK/state/daemon.state" 2>/dev/null | tail -n 1
}

alive() {
    [ -n "${1:-}" ] && [ "${1:-0}" -ne 0 ] 2>/dev/null && [ -d "/proc/$1" ]
}

await_state() {
    i=0
    while [ "$i" -lt "$3" ]; do
        [ "$(state_value "$1")" = "$2" ] && return 0
        i=$((i + 1))
        sleep 1
    done
    return 1
}

start_daemon() {
    "$WORK/bin/viper-daemon" \
        --state-root "$WORK/state" \
        --socket "viper4android.dd.driver.$TAG" \
        --app-socket "viper4android.dd.app.$TAG" \
        --owner-socket "viper4android.dd.owner.$TAG" \
        --owner-dex "$WORK/bin/viper-owner.dex" \
        --poll-ms 100 \
        >> "$WORK/daemon.log" 2>&1 &
    echo $!
}

cleanup() {
    for pid in $FIRST_DAEMON $SECOND_DAEMON; do
        alive "$pid" && kill "$pid" 2>/dev/null
    done
    sleep 2
    # The owner outlives its daemon by design now, so it has to be stopped here
    # explicitly or the probe would leak a root process holding an effect.
    for pid in $OWNER_PID $(state_value owner_pid); do
        alive "$pid" && kill "$pid" 2>/dev/null
    done
    sleep 2
    for pid in $OWNER_PID $(state_value owner_pid); do
        alive "$pid" && kill -9 "$pid" 2>/dev/null
    done
    rm -rf "$WORK"
    echo "leftover_processes=$(ps -A -o ARGS= 2>/dev/null | grep -c 'viper-daemon-death-chec[k]')"
}
trap cleanup EXIT

[ "$(id -u)" = "0" ] || { echo 'FAILED: must run as root' >&2; exit 1; }
[ -f "$BIN_DIR/viper-daemon_arm64-v8a" ] || { echo "FAILED: no daemon at $BIN_DIR" >&2; exit 1; }
[ -f "$BIN_DIR/viper-owner.dex" ] || { echo "FAILED: no owner dex at $BIN_DIR" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/bin" "$WORK/state"
cp "$BIN_DIR/viper-daemon_arm64-v8a" "$WORK/bin/viper-daemon"
cp "$BIN_DIR/viper-owner.dex" "$WORK/bin/viper-owner.dex"
chmod 0755 "$WORK/bin/viper-daemon"
chmod 0644 "$WORK/bin/viper-owner.dex"

BASELINE_MODULES=$(modules)
echo "baseline_modules=$BASELINE_MODULES"

FIRST_DAEMON=$(start_daemon)
echo "first_daemon=$FIRST_DAEMON"
if ! await_state owner_state owned 40; then
    fail "owner never reached owned state (last=$(state_value owner_state))"
    cat "$WORK/daemon.log" >&2
    exit "$STATUS"
fi

OWNER_PID=$(state_value owner_pid)
OWNER_EFFECT=$(state_value owner_effect_id)
OWNED_MODULES=$(modules)
echo "before_daemon_kill owner_pid=$OWNER_PID effect=$OWNER_EFFECT modules=$OWNED_MODULES"
[ "$OWNED_MODULES" -gt "$BASELINE_MODULES" ] || { fail 'owner effect never appeared'; exit "$STATUS"; }

# SIGKILL, not SIGTERM: an update or a crash gives the daemon no chance to hand
# the effect over, so that is the case that has to hold.
kill -9 "$FIRST_DAEMON"
sleep 5
AFTER_KILL_MODULES=$(modules)
echo "after_daemon_kill owner_alive=$(alive "$OWNER_PID" && echo 1 || echo 0) modules=$AFTER_KILL_MODULES"
alive "$OWNER_PID" || fail 'owner died with the daemon'
[ "$AFTER_KILL_MODULES" = "$OWNED_MODULES" ] \
    || fail "ViPER module count changed when the daemon died ($OWNED_MODULES -> $AFTER_KILL_MODULES)"

# init restarts the daemon; the replacement must adopt the running owner rather
# than spawn a second one and create a duplicate effect.
SECOND_DAEMON=$(start_daemon)
echo "second_daemon=$SECOND_DAEMON"
if ! await_state owner_state owned 40; then
    fail "restarted daemon never reported an owner (last=$(state_value owner_state))"
    cat "$WORK/daemon.log" >&2
    exit "$STATUS"
fi
sleep 2

ADOPTED_PID=$(state_value owner_pid)
ADOPTED_EFFECT=$(state_value owner_effect_id)
ADOPTED_MODULES=$(modules)
echo "after_daemon_restart owner_pid=$ADOPTED_PID effect=$ADOPTED_EFFECT modules=$ADOPTED_MODULES spawn_failures=$(state_value owner_spawn_failures)"

[ "$ADOPTED_PID" = "$OWNER_PID" ] \
    || fail "restarted daemon did not adopt the running owner ($OWNER_PID -> $ADOPTED_PID)"
[ "$ADOPTED_EFFECT" = "$OWNER_EFFECT" ] \
    || fail "adopted owner reports a different effect id ($OWNER_EFFECT -> $ADOPTED_EFFECT)"
# One module: not zero (handle lost) and not two (duplicate owner spawned).
[ "$ADOPTED_MODULES" = "$OWNED_MODULES" ] \
    || fail "ViPER module count changed across the daemon restart ($OWNED_MODULES -> $ADOPTED_MODULES)"
alive "$ADOPTED_PID" || fail 'adopted owner is not alive'

[ "$STATUS" -eq 0 ] && echo 'DAEMON_DEATH_VERIFY=PASS'
exit "$STATUS"
