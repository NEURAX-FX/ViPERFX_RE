#!/bin/sh
# Verifies that this KernelSU build actually picks a module's initrc/*.rc file up
# into the generated modules.rc. Run as root.
#
# Reversible and reboot-free: it creates a scratch module that ships nothing but
# module.prop + initrc/, runs `ksud initrc refresh`, inspects the generated file,
# then removes the scratch module and refreshes again. The installed
# ViPER4Android-RE module is never touched. Injection into init.rc happens only
# at boot, so nothing running is affected either way.
#
# Usage: su -c "sh tests/module/probe_ksu_initrc.sh <path-to-viper-daemon.rc>"
RC_SRC="${1:?usage: probe_ksu_initrc.sh <viper-daemon.rc>}"
PROBE_ID=viper_initrc_probe
PROBE_DIR=/data/adb/modules/$PROBE_ID
GENERATED=/metadata/watchdog/ksu/modules.rc
STATUS=0

fail() {
    echo "FAILED: $1" >&2
    STATUS=1
}

cleanup() {
    rm -rf "$PROBE_DIR"
    /data/adb/ksud initrc refresh >/dev/null 2>&1
    if [ -d "$PROBE_DIR" ]; then
        echo "WARNING: scratch module still present at $PROBE_DIR" >&2
    else
        echo "cleanup: scratch module removed"
    fi
    if grep -q "$PROBE_ID" "$GENERATED" 2>/dev/null; then
        fail "generated modules.rc still references the scratch module after cleanup"
    else
        echo "cleanup: generated modules.rc no longer references the probe"
    fi
}
trap cleanup EXIT INT TERM

[ -f "$RC_SRC" ] || { echo "FAILED: no rc file at $RC_SRC" >&2; exit 1; }
[ -x /data/adb/ksud ] || { echo "FAILED: no ksud" >&2; exit 1; }

echo "--- baseline generated modules.rc ---"
BASELINE_BYTES=$(stat -c %s "$GENERATED" 2>/dev/null || echo missing)
echo "bytes=$BASELINE_BYTES"
if grep -q "viper_daemon" "$GENERATED" 2>/dev/null; then
    fail "baseline modules.rc already defines viper_daemon"
fi

echo "--- installing scratch module (module.prop + initrc only) ---"
mkdir -p "$PROBE_DIR/initrc"
cat > "$PROBE_DIR/module.prop" <<EOF
id=$PROBE_ID
name=ViPER initrc probe
version=probe
versionCode=1
author=probe
description=Temporary probe verifying KernelSU initrc injection. Safe to delete.
EOF
# skip_mount: the probe must not overlay anything onto /system.
touch "$PROBE_DIR/skip_mount"
cp "$RC_SRC" "$PROBE_DIR/initrc/viper-daemon.rc"
chmod 0644 "$PROBE_DIR/module.prop" "$PROBE_DIR/initrc/viper-daemon.rc"

echo "--- ksud initrc refresh ---"
/data/adb/ksud initrc refresh 2>&1 | head -5

echo "--- generated modules.rc after refresh ---"
stat -c "bytes=%s" "$GENERATED" 2>/dev/null || fail "generated modules.rc missing"
if grep -q "service viper_daemon" "$GENERATED" 2>/dev/null; then
    echo "PICKED UP: service viper_daemon is present in $GENERATED"
    grep -n "viper_daemon" "$GENERATED" | head -10
else
    fail "service viper_daemon was NOT injected into $GENERATED"
fi
grep -q "on property:sys.boot_completed=1" "$GENERATED" 2>/dev/null \
    || fail "boot_completed trigger missing from generated rc"
grep -q "class late_start" "$GENERATED" 2>/dev/null \
    || fail "class late_start missing from generated rc"

if [ "$STATUS" -eq 0 ]; then
    echo "KernelSU initrc pickup verified"
else
    echo "KernelSU initrc pickup NOT verified"
fi
exit "$STATUS"
