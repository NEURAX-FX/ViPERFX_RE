#!/system/bin/sh
# Checks the owner recovers when AudioFlinger itself restarts.
#
# An AudioEffect handle is a client reference into audioserver. When audioserver
# dies every handle dies with it, including the owner's. The failure this guards
# against is silent: the owner process survives, so the daemon keeps publishing
# owner_state=owned and a stale effect id, the App sees "a handle exists" and stays
# out of the way, and nothing is processing audio at all.
#
# Runs an isolated daemon on unique abstract socket names and its own state root.
#
# NOTE: restarting audioserver interrupts audio system-wide for a few seconds. It
# is a normal, recoverable Android operation - init owns audioserver and respawns
# it - but it is not silent, so this script is separate from the other acceptance
# checks rather than folded into them.
#
# Usage (as root, on the device):
#   sh tests/owner/verify_audioserver_restart.sh
set -u

REPO_DIR="${REPO_DIR:-/data/data/com.tom.rv2ide/files/home/ViPERFX_RE}"
BIN_DIR="${BIN_DIR:-$REPO_DIR/out/magisk_module/common/bin}"
WORK=/data/local/tmp/viper-audioserver-check
VIPER_UUID=90380da3-8536-4744-a6a3-5731970e640f
STATUS=0
DAEMON_PID=
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

audioserver_pid() {
    pidof audioserver 2>/dev/null | awk '{ print $1 }'
}

cleanup() {
    alive "$DAEMON_PID" && kill "$DAEMON_PID" 2>/dev/null
    sleep 2
    for pid in $OWNER_PID $(state_value owner_pid); do
        alive "$pid" && kill "$pid" 2>/dev/null
    done
    sleep 1
    for pid in $OWNER_PID $(state_value owner_pid); do
        alive "$pid" && kill -9 "$pid" 2>/dev/null
    done
    rm -rf "$WORK"
    echo "leftover_processes=$(ps -A -o ARGS= 2>/dev/null | grep -c 'viper-audioserver-chec[k]')"
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
AUDIOSERVER_BEFORE=$(audioserver_pid)
echo "baseline_modules=$BASELINE_MODULES audioserver_before=${AUDIOSERVER_BEFORE:-none}"
[ -n "${AUDIOSERVER_BEFORE:-}" ] || { fail 'audioserver is not running'; exit "$STATUS"; }

"$WORK/bin/viper-daemon" \
    --state-root "$WORK/state" \
    --socket "viper4android.as.driver.$TAG" \
    --app-socket "viper4android.as.app.$TAG" \
    --owner-socket "viper4android.as.owner.$TAG" \
    --owner-dex "$WORK/bin/viper-owner.dex" \
    --poll-ms 100 \
    > "$WORK/daemon.log" 2>&1 &
DAEMON_PID=$!

i=0
while [ "$i" -lt 40 ]; do
    [ "$(state_value owner_state)" = "owned" ] && break
    i=$((i + 1))
    sleep 1
done
OWNER_PID=$(state_value owner_pid)
OWNER_EFFECT=$(state_value owner_effect_id)
OWNED_MODULES=$(modules)
echo "before_restart owner_pid=$OWNER_PID effect=$OWNER_EFFECT modules=$OWNED_MODULES"
[ "$(state_value owner_state)" = "owned" ] || { fail 'owner never took the handle'; exit "$STATUS"; }
[ "$OWNED_MODULES" -gt "$BASELINE_MODULES" ] || { fail 'owner effect never appeared'; exit "$STATUS"; }

# init owns audioserver and respawns it. Every AudioEffect client reference in the
# system dies here, including the owner's.
kill -9 "$AUDIOSERVER_BEFORE"

i=0
while [ "$i" -lt 60 ]; do
    AUDIOSERVER_AFTER=$(audioserver_pid)
    if [ -n "${AUDIOSERVER_AFTER:-}" ] && [ "${AUDIOSERVER_AFTER:-}" != "$AUDIOSERVER_BEFORE" ]; then
        break
    fi
    i=$((i + 1))
    sleep 1
done
AUDIOSERVER_AFTER=$(audioserver_pid)
echo "audioserver_after=${AUDIOSERVER_AFTER:-none}"
[ -n "${AUDIOSERVER_AFTER:-}" ] || { fail 'audioserver did not come back'; exit "$STATUS"; }

# Give the owner and the supervisor time to notice and rebuild.
i=0
while [ "$i" -lt 60 ]; do
    RECOVERED_MODULES=$(modules)
    [ "$RECOVERED_MODULES" -ge "$OWNED_MODULES" ] && break
    i=$((i + 1))
    sleep 1
done

RECOVERED_MODULES=$(modules)
RECOVERED_PID=$(state_value owner_pid)
RECOVERED_EFFECT=$(state_value owner_effect_id)
echo "after_restart owner_state=$(state_value owner_state) pid=$RECOVERED_PID effect=$RECOVERED_EFFECT modules=$RECOVERED_MODULES restarts=$(state_value owner_restarts)"

# The load-bearing assertion: a module must be present again. An owner still
# claiming `owned` with no module in AudioFlinger is the silent-failure case, and
# it also keeps the App from falling back.
[ "$RECOVERED_MODULES" -ge "$OWNED_MODULES" ] \
    || fail "ViPER module did not come back after audioserver restart ($OWNED_MODULES -> $RECOVERED_MODULES)"
[ "$(state_value owner_state)" = "owned" ] \
    || fail "owner state is $(state_value owner_state) after audioserver restart"
[ "${RECOVERED_EFFECT:-0}" != "0" ] || fail 'owner reports effect id 0 after audioserver restart'
alive "$RECOVERED_PID" || fail 'no live owner process after audioserver restart'

[ "$STATUS" -eq 0 ] && echo 'AUDIOSERVER_RESTART_VERIFY=PASS'
exit "$STATUS"
