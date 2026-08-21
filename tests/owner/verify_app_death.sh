#!/system/bin/sh
# Acceptance check: killing the App must not take ViPER's audio effect with it.
#
# This is the whole point of the owner process, so it must be verified on the
# production abstract sockets and the production state root. The App reads that
# state root to decide whether to create its own fallback AudioEffect; running on
# scratch socket names would leave the App talking to a different daemon and
# prove nothing.
#
# Because it takes over the production sockets, the script backs up the daemon
# state directory, stops the init service, and restores both unconditionally on
# exit.
#
# Usage (as root, on the device):
#   sh tests/owner/verify_app_death.sh
#
# Environment:
#   BIN_DIR  directory holding viper-daemon_arm64-v8a and viper-owner.dex
#   APK      debug APK to install and kill
#   STATE    production daemon state root
set -u

REPO_DIR="${REPO_DIR:-/data/data/com.tom.rv2ide/files/home/ViPERFX_RE}"
BIN_DIR="${BIN_DIR:-$REPO_DIR/out/magisk_module/common/bin}"
APK="${APK:-/data/data/com.tom.rv2ide/files/home/ViPER4Android/app/build/outputs/apk/debug/app-debug.apk}"
STATE="${STATE:-/data/adb/viper4android}"
PACKAGE="${PACKAGE:-com.llsl.viper4android}"
WORK=/data/local/tmp/viper-app-death-check
BACKUP="$WORK/state-backup"
VIPER_UUID=90380da3-8536-4744-a6a3-5731970e640f
STATUS=0
DAEMON_PID=
OWNER_PID=
INITIAL_SERVICE=$(getprop init.svc.viper_daemon 2>/dev/null || true)

fail() {
    echo "FAILED: $1" >&2
    STATUS=1
}

modules() {
    dumpsys media.audio_flinger 2>/dev/null | grep -c "$VIPER_UUID"
}

state_value() {
    sed -n "s/^$1=//p" "$STATE/daemon.state" 2>/dev/null | tail -n 1
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

cleanup() {
    alive "$DAEMON_PID" && kill "$DAEMON_PID" 2>/dev/null
    sleep 2
    alive "$OWNER_PID" && kill "$OWNER_PID" 2>/dev/null
    sleep 1
    alive "$OWNER_PID" && kill -9 "$OWNER_PID" 2>/dev/null

    # Restore the exact pre-test state directory before restarting the init
    # service, so the real daemon never observes a half-restored directory.
    if [ -d "$BACKUP" ]; then
        rm -rf "$STATE"
        cp -a "$BACKUP" "$STATE"
        chmod 0700 "$STATE" 2>/dev/null || true
    fi
    [ "$INITIAL_SERVICE" = "running" ] && start viper_daemon 2>/dev/null
    am force-stop "$PACKAGE" 2>/dev/null
    rm -rf "$WORK"
    echo "restored_init_service=$(getprop init.svc.viper_daemon 2>/dev/null || true)"
}
trap cleanup EXIT

[ "$(id -u)" = "0" ] || { echo 'FAILED: must run as root' >&2; exit 1; }
[ -f "$BIN_DIR/viper-daemon_arm64-v8a" ] || { echo "FAILED: no daemon at $BIN_DIR" >&2; exit 1; }
[ -f "$BIN_DIR/viper-owner.dex" ] || { echo "FAILED: no owner dex at $BIN_DIR" >&2; exit 1; }
[ -f "$APK" ] || { echo "FAILED: no APK at $APK" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK"
cp -a "$STATE" "$BACKUP"
cp "$BIN_DIR/viper-daemon_arm64-v8a" "$WORK/viper-daemon"
cp "$BIN_DIR/viper-owner.dex" "$WORK/viper-owner.dex"
chmod 0755 "$WORK/viper-daemon"
chmod 0644 "$WORK/viper-owner.dex"

# Take over the default sockets so the App under test talks to this build.
stop viper_daemon 2>/dev/null
sleep 2

"$WORK/viper-daemon" \
    --state-root "$STATE" \
    --owner-dex "$WORK/viper-owner.dex" \
    --poll-ms 100 \
    > "$WORK/daemon.log" 2>&1 &
DAEMON_PID=$!
echo "daemon_pid=$DAEMON_PID"

if ! await_state owner_state owned 40; then
    fail "owner never reached owned state (last=$(state_value owner_state))"
    cat "$WORK/daemon.log" >&2
    exit "$STATUS"
fi

OWNER_PID=$(state_value owner_pid)
OWNER_EFFECT=$(state_value owner_effect_id)
OWNER_MODULES=$(modules)
echo "owner_before_app pid=$OWNER_PID effect=$OWNER_EFFECT modules=$OWNER_MODULES"
alive "$OWNER_PID" || { fail 'owner pid is not alive'; exit "$STATUS"; }
[ "${OWNER_EFFECT:-0}" != "0" ] || { fail 'owner reported effect id 0'; exit "$STATUS"; }

pm install -r "$APK" >/dev/null 2>&1 || fail 'APK install failed'
am force-stop "$PACKAGE" 2>/dev/null
monkey -p "$PACKAGE" 1 >/dev/null 2>&1
i=0
while [ "$i" -lt 20 ]; do
    APP_PID=$(pidof "$PACKAGE" 2>/dev/null || true)
    [ -n "${APP_PID:-}" ] && break
    i=$((i + 1))
    sleep 1
done
APP_PID=$(pidof "$PACKAGE" 2>/dev/null || true)
echo "app_pid=${APP_PID:-none}"
[ -n "${APP_PID:-}" ] || { fail 'App did not start'; exit "$STATUS"; }
sleep 4

# A duplicate module here would mean the App created its own effect next to the
# daemon-owned one, which is exactly the behaviour the owner replaces.
WITH_APP_MODULES=$(modules)
echo "with_app owner_pid=$(state_value owner_pid) effect=$(state_value owner_effect_id) modules=$WITH_APP_MODULES"
[ "$(state_value owner_pid)" = "$OWNER_PID" ] || fail 'App displaced the owner'
[ "$(state_value owner_effect_id)" = "$OWNER_EFFECT" ] || fail 'App changed the owner effect id'
[ "$WITH_APP_MODULES" = "$OWNER_MODULES" ] || fail 'App created a duplicate ViPER module'

# force-stop is more deterministic than SIGKILL for a package that may run more
# than one process.
am force-stop "$PACKAGE"
sleep 4
AFTER_MODULES=$(modules)
AFTER_APP=$(pidof "$PACKAGE" 2>/dev/null || true)
echo "after_app_kill owner_state=$(state_value owner_state) pid=$(state_value owner_pid) effect=$(state_value owner_effect_id) modules=$AFTER_MODULES app=${AFTER_APP:-none} original_app=$APP_PID"

# ViperService returns START_STICKY, so Android may already have restarted the
# package under a new pid. What must hold is that the process which owned the
# App-side handles is gone; asserting the package has no process at all would be
# a race against that restart, not a property of the owner.
alive "$APP_PID" && fail 'the original App process survived force-stop'
[ "${AFTER_APP:-}" != "$APP_PID" ] || fail 'App pid unchanged after force-stop'
[ "$(state_value owner_state)" = "owned" ] || fail 'owner left owned state after App death'
[ "$(state_value owner_pid)" = "$OWNER_PID" ] || fail 'owner pid changed after App death'
[ "$(state_value owner_effect_id)" = "$OWNER_EFFECT" ] || fail 'owner effect id changed after App death'
# Covers both directions: no handle lost with the App, and no duplicate created
# by a sticky restart.
[ "$AFTER_MODULES" = "$OWNER_MODULES" ] || fail 'ViPER module count changed after App death'
alive "$OWNER_PID" || fail 'owner process died with the App'

# Surviving the App is necessary but not sufficient: the graph must still process
# audio. The probe generates its own tone and reads the driver's edge-based
# PARAM_GET_STREAMING, so a 1 here is direct evidence that frames moved through
# ViPER's DSP with the App gone. Skipped rather than failed when the probe dex was
# not built, because that is a missing tool, not a product defect.
PROBE_DEX="$REPO_DIR/out/probe/streaming-probe.dex"
if [ -f "$PROBE_DEX" ]; then
    cp "$PROBE_DEX" "$WORK/streaming-probe.dex"
    chmod 0644 "$WORK/streaming-probe.dex"
    CLASSPATH="$WORK/streaming-probe.dex" \
        app_process64 "$WORK" com.llsl.viper4android.probe.StreamingProbe \
        > "$WORK/probe.log" 2>&1
    PROBE_EXIT=$?
    STREAMING=$(sed -n 's/^streaming=//p' "$WORK/probe.log" | tail -n 1)
    echo "after_app_kill_streaming=${STREAMING:-none} probe_exit=$PROBE_EXIT"
    [ "${STREAMING:-0}" = "1" ] \
        || fail "driver stopped processing after App death (streaming=${STREAMING:-none})"
    # The probe attaches its own handle to measure; it must not have displaced the
    # owner or left a second module behind.
    [ "$(state_value owner_pid)" = "$OWNER_PID" ] || fail 'probe displaced the owner'
    [ "$(modules)" = "$OWNER_MODULES" ] || fail 'probe leaked a ViPER module'
else
    echo 'after_app_kill_streaming=skipped (run tests/owner/build_streaming_probe.sh)'
fi

[ "$STATUS" -eq 0 ] && echo 'APP_DEATH_VERIFY=PASS'
exit "$STATUS"
