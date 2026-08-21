#!/bin/sh
# Verifies the packaged module ships the ART effect owner, installs it where init
# can reach it, and wires the daemon to it.
#
# The owner is what keeps audio processing alive when the App dies, so a package
# that omits it silently degrades to the old App-owned behaviour.
#
# Usage: sh tests/module/OwnerPackagingTest.sh [module-out-dir]
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

has() {
    [ -f "$1" ] && grep -q -- "$2" "$1"
}

# Ignores comments and blank lines: a directive named in prose is not a directive.
has_effective() {
    [ -f "$1" ] || return 1
    sed -e 's/#.*$//' -e '/^[[:space:]]*$/d' "$1" | grep -q -- "$2"
}

reject() {
    if has_effective "$1" "$2"; then fail "$3"; fi
}

[ -d "$MODULE_OUT" ] || { echo "FAILED: no module output at $MODULE_OUT (run 'make module')" >&2; exit 1; }
echo "module_out=$MODULE_OUT"

echo "--- owner dex is packaged ---"
DEX="$MODULE_OUT/common/bin/viper-owner.dex"
require_file "$DEX"
# A dex begins with the magic "dex\n". An empty or text placeholder would install
# fine and then fail at runtime inside app_process, which is far harder to
# attribute than a packaging failure.
if [ -f "$DEX" ]; then
    MAGIC=$(head -c 4 "$DEX" | tr -d '\0')
    case "$MAGIC" in
        dex*) ;;
        *) fail "owner dex is not a dex file (magic='$MAGIC')" ;;
    esac
    SIZE=$(wc -c < "$DEX")
    [ "$SIZE" -gt 512 ] || fail "owner dex is implausibly small ($SIZE bytes)"
fi

echo "--- install script persists the owner outside common/ ---"
INSTALL="$MODULE_OUT/common/install.sh"
require_file "$INSTALL"
# functions.sh cleanup() deletes $MODPATH/common after install, so the dex has to
# be copied to a persistent location exactly like the daemon binary.
grep -q 'cp_ch .*common/bin/viper-owner\.dex \$MODPATH/bin/viper-owner\.dex' "$INSTALL" \
    || fail "install.sh does not copy the owner dex to \$MODPATH/bin"
# The owner runs as root under app_process; the dex only needs to be readable.
grep -q 'viper-owner\.dex .*0644' "$INSTALL" \
    || fail "install.sh does not install the owner dex with mode 0644"

echo "--- init passes the owner dex to the daemon ---"
RC="$MODULE_OUT/initrc/viper-daemon.rc"
require_file "$RC"
MODID="$(sed -n 's/^id=//p' "$MODULE_OUT/module.prop")"
has_effective "$RC" -- "--owner-dex" || fail "init service does not pass --owner-dex"
has_effective "$RC" "/data/adb/modules/$MODID/bin/viper-owner.dex" \
    || fail "init service does not point at the installed owner dex"
# init has no $MODDIR, so every path in the stanza must be literal.
reject "$RC" 'MODDIR' "init service uses \$MODDIR, which init cannot expand"

echo "--- uninstall removes the owner and its state ---"
UNINSTALL="$MODULE_OUT/uninstall.sh"
require_file "$UNINSTALL"
has "$UNINSTALL" "owner" || fail "uninstall.sh does not clean up owner artifacts"
# User snapshots must survive a module uninstall.
reject "$UNINSTALL" "current.snapshot" "uninstall.sh deletes user snapshots"

echo "--- packaged shell scripts parse ---"
for script in "$INSTALL" "$UNINSTALL"; do
    [ -f "$script" ] || continue
    sh -n "$script" 2>/dev/null || fail "shell syntax error in $script"
done

if [ "$STATUS" -eq 0 ]; then
    echo "owner packaging tests passed"
else
    echo "owner packaging tests failed" >&2
fi
exit "$STATUS"
