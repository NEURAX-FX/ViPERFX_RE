#!/system/bin/sh
# Device check for the daemon-supervised ART effect owner.
#
# Runs an isolated daemon on unique abstract socket names and its own state root,
# so the installed init-supervised daemon and any live audio stay untouched. It
# proves: the daemon spawns the owner, the owner takes session 0 and reports a
# real AudioFlinger effect id, the effect appears in AudioFlinger, and killing
# the owner produces a bounded respawn with a fresh effect id.
#
# Usage (as root, on the device):
#   sh tests/owner/verify_owner_device.sh
#
# Environment:
#   BIN_DIR  directory holding viper-daemon_arm64-v8a and viper-owner.dex
#            (default: the packaged module output under REPO_DIR)
#   REPO_DIR repository root on the device
set -u

REPO_DIR="${REPO_DIR:-/data/data/com.tom.rv2ide/files/home/ViPERFX_RE}"
BIN_DIR="${BIN_DIR:-$REPO_DIR/out/magisk_module/common/bin}"
WORK=/data/local/tmp/viper-owner-device-check
VIPER_UUID=90380da3-8536-4744-a6a3-5731970e640f
STATUS=0
DAEMON_PID=
OWNER_PID=
RESPAWNED_PID=

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

# Waits for a state key to reach a value instead of sleeping a fixed interval:
# owner startup includes an ART boot, which varies widely between devices.
await_state() {
    key=$1
    want=$2
    limit=$3
    i=0
    while [ "$i" -lt "$limit" ]; do
        [ "$(state_value "$key")" = "$want" ] && return 0
        i=$((i + 1))
        sleep 1
    done
    return 1
}

cleanup() {
    # Only this script's daemon pid is signalled. Its owner exits on socket EOF;
    # anything still alive after the bounded wait is force-stopped.
    alive "$DAEMON_PID" && kill "$DAEMON_PID" 2>/dev/null
    sleep 2
    for pid in $OWNER_PID $RESPAWNED_PID; do
        alive "$pid" && kill "$pid" 2>/dev/null
    done
    sleep 1
    for pid in $OWNER_PID $RESPAWNED_PID; do
        alive "$pid" && kill -9 "$pid" 2>/dev/null
    done
    rm -rf "$WORK"
    echo "leftover_processes=$(ps -A -o ARGS= 2>/dev/null | grep -c 'viper-owner-device-chec[k]')"
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

# Unique socket names keep this daemon off the production driver/app/owner
# sockets, so the installed daemon keeps its driver connection.
TAG=$$
"$WORK/bin/viper-daemon" \
    --state-root "$WORK/state" \
    --socket "viper4android.check.driver.$TAG" \
    --app-socket "viper4android.check.app.$TAG" \
    --owner-socket "viper4android.check.owner.$TAG" \
    --owner-dex "$WORK/bin/viper-owner.dex" \
    --poll-ms 100 \
    > "$WORK/daemon.log" 2>&1 &
DAEMON_PID=$!
echo "daemon_pid=$DAEMON_PID"

if ! await_state owner_state owned 40; then
    fail "owner never reached owned state (last=$(state_value owner_state))"
    echo "daemon_log:"; cat "$WORK/daemon.log" >&2
    exit "$STATUS"
fi

OWNER_PID=$(state_value owner_pid)
OWNER_EFFECT=$(state_value owner_effect_id)
OWNER_CONTROL=$(state_value owner_has_control)
OWNED_MODULES=$(modules)
echo "owner_pid=$OWNER_PID owner_effect_id=$OWNER_EFFECT owner_has_control=$OWNER_CONTROL modules=$OWNED_MODULES"

alive "$OWNER_PID" || fail 'owner pid is not a live process'
[ "${OWNER_EFFECT:-0}" != "0" ] || fail 'owner reported effect id 0'
[ "$OWNER_CONTROL" = "1" ] || fail 'owner does not have effect control'
[ "$OWNED_MODULES" -gt "$BASELINE_MODULES" ] \
    || fail "owner effect absent from AudioFlinger (modules $OWNED_MODULES <= baseline $BASELINE_MODULES)"
[ "$(state_value owner_spawn_failures)" = "0" ] || fail 'daemon reported owner spawn failures'

# Killing the owner must produce exactly one bounded respawn with a new effect
# id: a supervisor that reuses the dead effect id would leave a stale handle.
kill -9 "$OWNER_PID"
i=0
while [ "$i" -lt 40 ]; do
    RESPAWNED_PID=$(state_value owner_pid)
    if [ "$(state_value owner_state)" = "owned" ] \
        && [ -n "$RESPAWNED_PID" ] && [ "$RESPAWNED_PID" != "$OWNER_PID" ]; then
        break
    fi
    i=$((i + 1))
    sleep 1
done
RESPAWNED_PID=$(state_value owner_pid)
RESPAWNED_EFFECT=$(state_value owner_effect_id)
RESTARTS=$(state_value owner_restarts)
RESPAWN_MODULES=$(modules)
echo "respawned_pid=$RESPAWNED_PID respawned_effect_id=$RESPAWNED_EFFECT restarts=$RESTARTS modules=$RESPAWN_MODULES"

[ "$(state_value owner_state)" = "owned" ] || fail 'owner did not return to owned after kill'
[ "$RESPAWNED_PID" != "$OWNER_PID" ] || fail 'owner pid unchanged after kill'
alive "$RESPAWNED_PID" || fail 'respawned owner is not alive'
[ "${RESPAWNED_EFFECT:-0}" != "0" ] || fail 'respawned owner reported effect id 0'
[ "$RESPAWNED_EFFECT" != "$OWNER_EFFECT" ] || fail 'respawned owner reused the dead effect id'
[ "${RESTARTS:-0}" -ge 1 ] || fail 'daemon did not count the owner restart'
[ "$RESPAWN_MODULES" -gt "$BASELINE_MODULES" ] || fail 'owner effect missing after respawn'

[ "$STATUS" -eq 0 ] && echo 'OWNER_DEVICE_VERIFY=PASS'
exit "$STATUS"
