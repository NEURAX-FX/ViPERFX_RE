#!/bin/sh
# Verifies the packaged module ships an init-owned daemon service, a start
# trigger, and keeps the existing driver payload intact.
#
# Usage: sh tests/module/DaemonInitServiceTest.sh [module-out-dir]
set -e

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MODULE_OUT="${1:-$REPO_ROOT/out/magisk_module}"
STATUS=0

fail() {
    echo "FAILED: $1" >&2
    STATUS=1
}

require_file() {
    [ -f "$1" ] || fail "missing file: $1"
}

# Absence is reported by require_file; this only answers "does it contain".
has() {
    [ -f "$1" ] && grep -q -- "$2" "$1"
}

# Same, but ignores comments and blank lines: a directive named in prose is not
# a directive. `install_script` also strips comments on-device, so the effective
# content is what matters.
has_effective() {
    [ -f "$1" ] || return 1
    sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$1" | grep -q -- "$2"
}

# `has ... && fail` would abort the whole script under `set -e` whenever the
# pattern is absent. These wrappers keep the failure local.
reject() {
    if has_effective "$1" "$2"; then fail "$3"; fi
}

reject_raw() {
    if [ -f "$1" ] && grep -qE -- "$2" "$1"; then fail "$3"; fi
}

[ -d "$MODULE_OUT" ] || { echo "FAILED: no module output at $MODULE_OUT (run 'make module')" >&2; exit 1; }
echo "module_out=$MODULE_OUT"

echo "--- daemon binary is packaged per ABI ---"
DAEMONS=0
for candidate in "$MODULE_OUT"/common/bin/viper-daemon_*; do
    [ -f "$candidate" ] && DAEMONS=$((DAEMONS + 1))
done
[ "$DAEMONS" -ge 1 ] || fail "no viper-daemon binary under common/bin"

echo "--- init service definition ---"
RC="$MODULE_OUT/initrc/viper-daemon.rc"
require_file "$RC"
has_effective "$RC" "service viper_daemon" || fail "no 'service viper_daemon' stanza"
has_effective "$RC" "class late_start" || fail "service is not in class late_start"
has_effective "$RC" "disabled" || fail "service is not 'disabled' (must be started explicitly)"
has_effective "$RC" "user root" || fail "service does not run as user root"
has_effective "$RC" "group root" || fail "service does not run as group root"
has_effective "$RC" "on property:sys.boot_completed=1" || fail "no boot_completed start trigger"
# oneshot would stop init from restarting a crashed daemon.
reject "$RC" "oneshot" "service is marked oneshot; init must restart an unexpected exit"
# init has no $MODDIR, so the exec path must be literal and match module.prop id.
MODID="$(sed -n 's/^id=//p' "$MODULE_OUT/module.prop")"
has_effective "$RC" "/data/adb/modules/$MODID/bin/viper-daemon" \
    || fail "service path does not point at /data/adb/modules/$MODID/bin/viper-daemon"

echo "--- start trigger scripts ---"
BOOT="$MODULE_OUT/boot-completed.sh"
SERVICE="$MODULE_OUT/service.sh"
START="$MODULE_OUT/daemon-start.sh"
require_file "$BOOT"
require_file "$SERVICE"
require_file "$START"
has_effective "$START" "start viper_daemon" || fail "helper does not ask init to start the service"
has_effective "$START" "init.svc.viper_daemon" || fail "helper does not check init service state"
# The daemon lifecycle belongs to init, not a shell restart loop.
reject "$START" "while true" "helper implements its own restart loop"
reject "$START" "bin/viper-daemon &" "helper launches the daemon directly instead of via init"
has "$BOOT" "daemon-start.sh" || fail "boot-completed.sh does not call the helper"
# Magisk has no boot-completed stage, so service.sh must wait for the property.
has "$SERVICE" "sys.boot_completed" || fail "service.sh does not wait for boot"
has "$SERVICE" "daemon-start.sh" || fail "service.sh does not call the helper"

echo "--- install script persists the daemon outside common/ ---"
INSTALL="$MODULE_OUT/common/install.sh"
require_file "$INSTALL"
# functions.sh cleanup() deletes $MODPATH/common after install.
has "$INSTALL" "bin/viper-daemon" || fail "install.sh does not install the daemon binary"
grep -q 'cp_ch .*common/bin/viper-daemon_\$ABI \$MODPATH/bin/viper-daemon' "$INSTALL" \
    || fail "install.sh does not copy the ABI-specific daemon to \$MODPATH/bin"

echo "--- uninstall cleanup ---"
UNINSTALL="$MODULE_OUT/uninstall.sh"
require_file "$UNINSTALL"
has "$UNINSTALL" "stop viper_daemon" || fail "uninstall.sh does not stop the daemon service"
has "$UNINSTALL" "daemon.state" || fail "uninstall.sh does not remove daemon runtime state"
# User snapshots and App storage must survive a module uninstall.
reject_raw "$UNINSTALL" "rm -rf +/data/adb/viper4android( |$|/)" \
    "uninstall.sh deletes user snapshots"
reject "$UNINSTALL" "current.snapshot" "uninstall.sh deletes user snapshots"
reject "$UNINSTALL" "/data/data/" "uninstall.sh touches App storage"

echo "--- existing driver payload preserved ---"
LIBS=0
for candidate in "$MODULE_OUT"/common/files/libv4a_re_*.so; do
    [ -f "$candidate" ] && LIBS=$((LIBS + 1))
done
[ "$LIBS" -ge 1 ] || fail "driver libraries are missing from the package"
require_file "$MODULE_OUT/post-fs-data.sh"
require_file "$MODULE_OUT/customize.sh"
require_file "$MODULE_OUT/module.prop"
has "$MODULE_OUT/post-fs-data.sh" "v4a_re" || fail "post-fs-data.sh lost its driver config patching"

echo "--- packaged shell scripts parse ---"
for script in "$BOOT" "$SERVICE" "$START" "$UNINSTALL" "$INSTALL" \
    "$MODULE_OUT/post-fs-data.sh"; do
    [ -f "$script" ] || continue
    sh -n "$script" || fail "syntax error in $script"
done

if [ "$STATUS" -eq 0 ]; then
    echo "daemon init service packaging tests passed"
else
    echo "daemon init service packaging tests FAILED"
fi
exit "$STATUS"
