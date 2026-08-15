#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
. "$ROOT/module/common/session0-pinner.sh"
TMP="${TMPDIR:-/tmp}/viper-session0-pinner-$$"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"

cp "$ROOT/tests/module/fixtures/audio_effects.xml" "$TMP/no-post.xml"
pin_viper_session0_effect "$TMP/no-post.xml"
validate_viper_session0_effect "$TMP/no-post.xml"
[ "$(grep -c '<apply effect="v4a_standard_re"/>' "$TMP/no-post.xml")" -eq 1 ]
pin_viper_session0_effect "$TMP/no-post.xml"
[ "$(grep -c '<apply effect="v4a_standard_re"/>' "$TMP/no-post.xml")" -eq 1 ]

cp "$ROOT/tests/module/fixtures/audio_effects-with-postprocess.xml" "$TMP/post.xml"
pin_viper_session0_effect "$TMP/post.xml"
validate_viper_session0_effect "$TMP/post.xml"
[ "$(grep -c '<apply effect="v4a_standard_re"/>' "$TMP/post.xml")" -eq 1 ]
grep -q '<apply effect="oem_music"/>' "$TMP/post.xml"

cp "$ROOT/tests/module/fixtures/audio_effects.conf" "$TMP/effects.conf"
pin_viper_session0_effect "$TMP/effects.conf"
validate_viper_session0_effect "$TMP/effects.conf"
[ "$(grep -c 'v4a_standard_re' "$TMP/effects.conf")" -eq 2 ]
pin_viper_session0_effect "$TMP/effects.conf"
[ "$(grep -c '# ViPER session0 pin' "$TMP/effects.conf")" -eq 1 ]

printf '%s\n' 'Session 0 pinner tests passed'
