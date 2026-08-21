#!/bin/sh
# Proves the ART owner's Java codec and the daemon's C++ codec produce identical
# bytes, and that the owner dex builds and contains the owner classes.
#
# The owner and daemon codecs are written independently. A field-order or
# endianness drift between them would pass both unit suites and only fail on a
# device as a rejected frame, so the byte-level diff is the contract check.
#
# Usage: sh tests/owner/owner_wire_smoke.sh
#
# Environment:
#   BUILD_DIR   existing CMake build dir for the native emitter
#               (default /root/tmp/viperfx-clang)
#   ANDROID_JAR optional; when set together with D8_JAR the owner dex is built
#               and inspected as well
set -e

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-/root/tmp/viperfx-clang}"
JAVAC="${JAVAC:-javac}"
JAVA="${JAVA:-java}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STATUS=0

fail() {
    echo "FAILED: $1" >&2
    STATUS=1
}

# --- native vectors -------------------------------------------------------
if [ ! -d "$BUILD_DIR" ]; then
    echo "FAILED: no CMake build dir at $BUILD_DIR; configure it first" >&2
    exit 1
fi
cmake --build "$BUILD_DIR" -j1 --target owner_wire_vectors >"$WORK/build.log" 2>&1 \
    || { cat "$WORK/build.log" >&2; echo 'FAILED: owner_wire_vectors build' >&2; exit 1; }
"$BUILD_DIR/owner_wire_vectors" > "$WORK/native.txt" \
    || { echo 'FAILED: native emitter exited nonzero' >&2; exit 1; }

# --- java vectors from the production owner codec -------------------------
mkdir -p "$WORK/classes"
"$JAVAC" -d "$WORK/classes" \
    "$REPO_ROOT/owner/src/com/llsl/viper4android/owner/OwnerWire.java" \
    "$REPO_ROOT/tests/owner/OwnerWireVectors.java" >"$WORK/javac.log" 2>&1 \
    || { cat "$WORK/javac.log" >&2; echo 'FAILED: owner wire javac' >&2; exit 1; }
"$JAVA" -cp "$WORK/classes" com.llsl.viper4android.owner.OwnerWireVectors \
    > "$WORK/java.txt" \
    || { echo 'FAILED: java emitter exited nonzero' >&2; exit 1; }

# Both emitters must cover the whole message set, otherwise an empty diff would
# be meaningless.
EXPECTED_VECTORS=10
native_count=$(wc -l < "$WORK/native.txt" | tr -d ' ')
java_count=$(wc -l < "$WORK/java.txt" | tr -d ' ')
[ "$native_count" = "$EXPECTED_VECTORS" ] \
    || fail "native emitter produced $native_count vectors, expected $EXPECTED_VECTORS"
[ "$java_count" = "$EXPECTED_VECTORS" ] \
    || fail "java emitter produced $java_count vectors, expected $EXPECTED_VECTORS"

if diff -u "$WORK/native.txt" "$WORK/java.txt" > "$WORK/diff.txt"; then
    echo "wire_vectors_match=$native_count"
else
    cat "$WORK/diff.txt" >&2
    fail 'owner wire bytes differ between native and Java codecs'
fi

# Frames must be the 36-byte header plus the fixed payload sizes from the spec.
awk '{ print $1, length($2) / 2 }' "$WORK/native.txt" > "$WORK/sizes.txt"
check_size() {
    actual=$(awk -v n="$1" '$1 == n { print $2 }' "$WORK/sizes.txt")
    [ "$actual" = "$2" ] || fail "$1 frame is $actual bytes, expected $2"
}
check_size owner_hello 56            # 36 + 20
check_size owner_hello_ack 52        # 36 + 16
check_size own_session_hidl 52
check_size owned 52
check_size own_failed 52
check_size release_session 48        # 36 + 12
check_size released 48
check_size session_delta_appeared 52

# A truncated or CRC-broken frame must be rejected, not silently accepted.
cat > "$WORK/RejectCheck.java" <<'JAVA'
package com.llsl.viper4android.owner;

public final class RejectCheck {
    public static void main(String[] args) {
        byte[] frame = OwnerWire.frame(OwnerWire.OWNED, 1L, 1L, OwnerWire.owned(0, 7, true));
        expectReject("truncated frame", frame, frame.length - 1);
        byte[] corrupt = frame.clone();
        corrupt[corrupt.length - 1] ^= 0x40;
        expectReject("crc mismatch", corrupt, corrupt.length);
        byte[] badMagic = frame.clone();
        badMagic[0] = 'X';
        expectReject("bad magic", badMagic, badMagic.length);
        System.out.println("reject_checks=3");
    }

    private static void expectReject(String name, byte[] bytes, int length) {
        try {
            OwnerWire.decodeFrame(bytes, length);
        } catch (RuntimeException expected) {
            return;
        }
        System.err.println("FAILED: owner codec accepted " + name);
        System.exit(1);
    }
}
JAVA
"$JAVAC" -d "$WORK/classes" -cp "$WORK/classes" "$WORK/RejectCheck.java" \
    >"$WORK/javac2.log" 2>&1 \
    || { cat "$WORK/javac2.log" >&2; fail 'reject-check javac'; }
if [ -f "$WORK/classes/com/llsl/viper4android/owner/RejectCheck.class" ]; then
    "$JAVA" -cp "$WORK/classes" com.llsl.viper4android.owner.RejectCheck \
        || fail 'owner codec accepted a malformed frame'
fi

# --- optional dex packaging check -----------------------------------------
if [ -n "${ANDROID_JAR:-}" ] && [ -n "${D8_JAR:-}" ]; then
    OUT_DIR="$WORK/dex" ANDROID_JAR="$ANDROID_JAR" D8_JAR="$D8_JAR" \
        bash "$REPO_ROOT/owner/build-owner.sh" >"$WORK/dex.log" 2>&1 \
        || { cat "$WORK/dex.log" >&2; fail 'owner dex build'; }
    DEX="$WORK/dex/viper-owner.dex"
    if [ -f "$DEX" ]; then
        echo "owner_dex_bytes=$(wc -c < "$DEX" | tr -d ' ')"
        # The dex must actually carry the owner entry point and the effect owner.
        for class in OwnerMain EffectOwner SessionObserver OwnerWire; do
            grep -q "$class" "$DEX" \
                || fail "owner dex does not reference $class"
        done
    else
        fail "owner dex missing at $DEX"
    fi
else
    echo 'owner_dex_check=skipped (set ANDROID_JAR and D8_JAR to enable)'
fi

[ "$STATUS" -eq 0 ] && echo 'OWNER_WIRE_SMOKE=PASS'
exit "$STATUS"
