#!/system/bin/sh
# Acceptance check: the driver processes audio while no App is running.
#
# The owner holding a handle is necessary but not sufficient: the graph must
# actually run. PARAM_GET_STREAMING is edge-based in the driver - it compares the
# engine's processed-frame counter against the previous read - so a 1 is direct
# evidence that frames moved through ViPER's DSP, with the App force-stopped.
#
# Runs an isolated daemon on unique abstract socket names and its own state root,
# so the installed init-supervised daemon is untouched.
#
# Usage (as root, on the device):
#   sh tests/owner/verify_streaming_without_app.sh
#
# Environment:
#   ANDROID_JAR / D8_JAR  required, to build the probe dex
set -u

REPO_DIR="${REPO_DIR:-/data/data/com.tom.rv2ide/files/home/ViPERFX_RE}"
BIN_DIR="${BIN_DIR:-$REPO_DIR/out/magisk_module/common/bin}"
PACKAGE="${PACKAGE:-com.llsl.viper4android}"
WORK=/data/local/tmp/viper-streaming-check
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
    echo "leftover_processes=$(ps -A -o ARGS= 2>/dev/null | grep -c 'viper-streaming-chec[k]')"
}
trap cleanup EXIT

[ "$(id -u)" = "0" ] || { echo 'FAILED: must run as root' >&2; exit 1; }
[ -f "$BIN_DIR/viper-daemon_arm64-v8a" ] || { echo "FAILED: no daemon at $BIN_DIR" >&2; exit 1; }
[ -f "$BIN_DIR/viper-owner.dex" ] || { echo "FAILED: no owner dex at $BIN_DIR" >&2; exit 1; }
[ -f "$REPO_DIR/out/probe/streaming-probe.dex" ] \
    || { echo "FAILED: no probe dex; run tests/owner/build_streaming_probe.sh first" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/bin" "$WORK/state"
cp "$BIN_DIR/viper-daemon_arm64-v8a" "$WORK/bin/viper-daemon"
cp "$BIN_DIR/viper-owner.dex" "$WORK/bin/viper-owner.dex"
cp "$REPO_DIR/out/probe/streaming-probe.dex" "$WORK/bin/streaming-probe.dex"
chmod 0755 "$WORK/bin/viper-daemon"
chmod 0644 "$WORK/bin/viper-owner.dex" "$WORK/bin/streaming-probe.dex"

# The App must be absent: that is the condition under test.
am force-stop "$PACKAGE" 2>/dev/null
sleep 1
APP_PID=$(pidof "$PACKAGE" 2>/dev/null || true)
echo "app_pid=${APP_PID:-none}"
[ -z "${APP_PID:-}" ] || fail 'App is still running; this probe requires it absent'

"$WORK/bin/viper-daemon" \
    --state-root "$WORK/state" \
    --socket "viper4android.sc.driver.$TAG" \
    --app-socket "viper4android.sc.app.$TAG" \
    --owner-socket "viper4android.sc.owner.$TAG" \
    --owner-dex "$WORK/bin/viper-owner.dex" \
    --poll-ms 100 \
    > "$WORK/daemon.log" 2>&1 &
DAEMON_PID=$!
echo "daemon_pid=$DAEMON_PID"

i=0
while [ "$i" -lt 40 ]; do
    [ "$(state_value owner_state)" = "owned" ] && break
    i=$((i + 1))
    sleep 1
done
OWNER_PID=$(state_value owner_pid)
echo "owner_pid=$OWNER_PID owner_effect_id=$(state_value owner_effect_id) modules=$(modules)"
[ "$(state_value owner_state)" = "owned" ] || { fail 'owner never took the handle'; exit "$STATUS"; }

SESSIONS_BEFORE=$(state_value tracked_sessions)
echo "tracked_sessions_before=$SESSIONS_BEFORE"

# app_process, not the App: the probe is a bare ART process, so a 1 here cannot be
# explained by the App's own effect client.
#
# Backgrounded so the tone is still playing while tracked_sessions is sampled: the
# probe's own AudioTrack must appear as a SESSION_DELTA from the owner's
# AudioPlaybackCallback. That is the only place session tracking is observable now
# that the App defers it to the owner.
CLASSPATH="$WORK/bin/streaming-probe.dex" \
    app_process64 "$WORK/bin" com.llsl.viper4android.probe.StreamingProbe \
    > "$WORK/probe.log" 2>&1 &
PROBE_PID=$!

SESSIONS_PLAYING=$SESSIONS_BEFORE
i=0
while [ "$i" -lt 30 ]; do
    SESSIONS_PLAYING=$(state_value tracked_sessions)
    [ "${SESSIONS_PLAYING:-0}" -gt "${SESSIONS_BEFORE:-0}" ] && break
    alive "$PROBE_PID" || break
    i=$((i + 1))
    sleep 1
done
echo "tracked_sessions_playing=$SESSIONS_PLAYING"

wait "$PROBE_PID"
PROBE_EXIT=$?
cat "$WORK/probe.log"
STREAMING=$(sed -n 's/^streaming=//p' "$WORK/probe.log" | tail -n 1)
echo "probe_exit=$PROBE_EXIT streaming=${STREAMING:-none}"

[ "$PROBE_EXIT" -eq 0 ] || fail "probe exited $PROBE_EXIT"
[ "${STREAMING:-0}" = "1" ] || fail "driver did not report streaming (got ${STREAMING:-none})"
# Session tracking now lives in the owner, not the App. A count that never moves
# while a real stream is playing means the owner's AudioPlaybackCallback is not
# wired up, and the App has already stopped its own monitor.
[ "${SESSIONS_PLAYING:-0}" -gt "${SESSIONS_BEFORE:-0}" ] \
    || fail "owner reported no session delta for the playing stream (${SESSIONS_BEFORE:-0} -> ${SESSIONS_PLAYING:-0})"

# The App must still have been absent for the whole measurement.
AFTER_APP=$(pidof "$PACKAGE" 2>/dev/null || true)
echo "app_after=${AFTER_APP:-none}"
[ -z "${AFTER_APP:-}" ] || fail 'App started during the measurement; result is not App-free'
alive "$OWNER_PID" || fail 'owner died during the measurement'

[ "$STATUS" -eq 0 ] && echo 'STREAMING_WITHOUT_APP_VERIFY=PASS'
exit "$STATUS"
