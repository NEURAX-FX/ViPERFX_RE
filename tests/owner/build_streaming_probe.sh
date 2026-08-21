#!/usr/bin/env bash
# Builds the streaming probe dex used by tests/owner/verify_streaming_without_app.sh.
#
# Kept separate from owner/build-owner.sh: the probe is test-only and must never
# be packaged into the Magisk module.
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT_DIR=${OUT_DIR:-"$ROOT_DIR/out/probe"}
ANDROID_JAR=${ANDROID_JAR:?ANDROID_JAR must point to an Android API android.jar}
D8_JAR=${D8_JAR:?D8_JAR must point to d8.jar}
JAVAC=${JAVAC:-javac}
JAVA=${JAVA:-java}

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/classes" "$OUT_DIR/dex"
"$JAVAC" --release 17 -cp "$ANDROID_JAR" -d "$OUT_DIR/classes" \
  "$ROOT_DIR/tests/owner/StreamingProbe.java"
mapfile -t class_files < <(find "$OUT_DIR/classes" -type f -name '*.class' | sort)
"$JAVA" -cp "$D8_JAR" com.android.tools.r8.D8 \
  --lib "$ANDROID_JAR" \
  --min-api 21 \
  --output "$OUT_DIR/dex" \
  "${class_files[@]}"
cp "$OUT_DIR/dex/classes.dex" "$OUT_DIR/streaming-probe.dex"
printf 'probe dex: %s\n' "$OUT_DIR/streaming-probe.dex"
