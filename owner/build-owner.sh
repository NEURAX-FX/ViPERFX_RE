#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC_DIR="$ROOT_DIR/owner/src"
OUT_DIR=${OUT_DIR:-"$ROOT_DIR/out/owner"}
ANDROID_JAR=${ANDROID_JAR:?ANDROID_JAR must point to an Android API android.jar}
D8_JAR=${D8_JAR:?D8_JAR must point to d8.jar}
JAVAC=${JAVAC:-javac}
JAVA=${JAVA:-java}

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/classes" "$OUT_DIR/dex"
mapfile -t sources < <(find "$SRC_DIR" -type f -name '*.java' | sort)
if [ "${#sources[@]}" -eq 0 ]; then
  echo "no owner Java sources" >&2
  exit 1
fi
"$JAVAC" --release 17 -cp "$ANDROID_JAR" -d "$OUT_DIR/classes" "${sources[@]}"
mapfile -t class_files < <(find "$OUT_DIR/classes" -type f -name '*.class' | sort)
"$JAVA" -cp "$D8_JAR" com.android.tools.r8.D8 \
  --lib "$ANDROID_JAR" \
  --min-api 21 \
  --output "$OUT_DIR/dex" \
  "${class_files[@]}"
cp "$OUT_DIR/dex/classes.dex" "$OUT_DIR/viper-owner.dex"
printf 'owner dex: %s\n' "$OUT_DIR/viper-owner.dex"
