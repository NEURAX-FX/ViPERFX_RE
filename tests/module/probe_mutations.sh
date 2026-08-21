#!/bin/sh
# Mutation probe: confirms DaemonInitServiceTest.sh actually rejects defective
# packages instead of passing vacuously. Not part of the normal test run.
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$REPO_ROOT/out/magisk_module"
WORK="$(mktemp -d)"
PROBE="$WORK/mm"
STATUS=0

reset_probe() {
    rm -rf "$PROBE"
    cp -r "$SRC" "$PROBE"
}

# Each mutation must make the packaging test fail.
expect_fail() {
    if sh "$REPO_ROOT/tests/module/DaemonInitServiceTest.sh" "$PROBE" >/dev/null 2>&1; then
        echo "NOT DETECTED: $1"
        STATUS=1
    else
        echo "detected: $1"
    fi
}

# Baseline must pass, otherwise the probes prove nothing.
reset_probe
if sh "$REPO_ROOT/tests/module/DaemonInitServiceTest.sh" "$PROBE" >/dev/null 2>&1; then
    echo "baseline passes"
else
    echo "BASELINE FAILS: unmutated package is already rejected"
    rm -rf "$WORK"
    exit 1
fi

reset_probe
sed -i 's/^    disabled/    oneshot/' "$PROBE/initrc/viper-daemon.rc"
expect_fail "oneshot instead of disabled"

reset_probe
printf 'rm -rf /data/adb/viper4android\n' >> "$PROBE/uninstall.sh"
expect_fail "uninstall wipes user snapshots"

reset_probe
printf 'rm -f /data/data/com.viper4android/databases/state.db\n' >> "$PROBE/uninstall.sh"
expect_fail "uninstall touches App storage"

reset_probe
rm -f "$PROBE/daemon-start.sh"
expect_fail "missing start helper"

reset_probe
sed -i 's|/data/adb/modules/ViPER4Android-RE/bin/viper-daemon|$MODDIR/bin/viper-daemon|' \
    "$PROBE/initrc/viper-daemon.rc"
expect_fail "rc uses \$MODDIR, which init cannot expand"

reset_probe
sed -i 's/^    class late_start/    class main/' "$PROBE/initrc/viper-daemon.rc"
expect_fail "wrong service class"

reset_probe
rm -f "$PROBE"/common/bin/viper-daemon_*
expect_fail "daemon binary not packaged"

reset_probe
rm -f "$PROBE"/common/files/libv4a_re_*.so
expect_fail "driver libraries dropped"

reset_probe
sed -i 's/^start viper_daemon$/$MODDIR\/bin\/viper-daemon \&/' "$PROBE/daemon-start.sh"
expect_fail "helper launches daemon directly instead of via init"

reset_probe
sed -i 's|^cp_ch -n $MODPATH/common/bin/viper-daemon_$ABI.*|true|' "$PROBE/common/install.sh"
expect_fail "install.sh does not persist the daemon binary"

reset_probe
printf 'if [\n' >> "$PROBE/daemon-start.sh"
expect_fail "syntax error in packaged script"

# Owner packaging probes. Same contract: each mutation must be rejected, so the
# owner test cannot pass vacuously either.
expect_owner_fail() {
    if sh "$REPO_ROOT/tests/module/OwnerPackagingTest.sh" "$PROBE" >/dev/null 2>&1; then
        echo "NOT DETECTED: $1"
        STATUS=1
    else
        echo "detected: $1"
    fi
}

reset_probe
if sh "$REPO_ROOT/tests/module/OwnerPackagingTest.sh" "$PROBE" >/dev/null 2>&1; then
    echo "owner baseline passes"
else
    echo "OWNER BASELINE FAILS: unmutated package is already rejected"
    STATUS=1
fi

reset_probe
rm -f "$PROBE/common/bin/viper-owner.dex"
expect_owner_fail "owner dex not packaged"

reset_probe
# A text placeholder installs fine and then fails inside app_process at runtime.
printf 'not a dex\n' > "$PROBE/common/bin/viper-owner.dex"
expect_owner_fail "owner dex is not a real dex"

reset_probe
# The line is indented inside an `if`, so the anchor must allow leading space.
sed -i 's|^[[:space:]]*cp_ch -n $MODPATH/common/bin/viper-owner\.dex.*|true|' \
    "$PROBE/common/install.sh"
expect_owner_fail "install.sh does not persist the owner dex"

reset_probe
sed -i 's| --owner-dex [^ ]*||' "$PROBE/initrc/viper-daemon.rc"
expect_owner_fail "init service does not pass the owner dex"

rm -rf "$WORK"
if [ "$STATUS" -eq 0 ]; then
    echo "all mutations detected"
else
    echo "SOME MUTATIONS NOT DETECTED"
fi
exit "$STATUS"
