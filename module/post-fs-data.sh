#!/system/bin/sh

. "$MODPATH/common/session0-pinner.sh"

LIBPATCH="$(cat "$MODPATH/libpatch.txt")"
CFGS="$(find /odm /system /vendor -type f \( -name '*audio_effects*.conf' -o -name '*audio_effects*.xml' \) 2>/dev/null)"

for FILE in $CFGS; do
  BACKUP="${FILE}.viper-session0-backup"
  cp -f "$FILE" "$BACKUP" || continue
  case "$FILE" in
    *.conf)
      sed -i "/v4a_standard_re {/,/}/d" "$FILE"
      sed -i "/v4a_re {/,/}/d" "$FILE"
      sed -i "s|^effects {|effects {\n  v4a_standard_re {\n    library v4a_re\n    uuid 90380da3-8536-4744-a6a3-5731970e640f\n  }|g" "$FILE"
      sed -i "s|^libraries {|libraries {\n  v4a_re {\n    path $LIBPATCH/lib/soundfx/libv4a_re.so\n  }|g" "$FILE"
      ;;
    *.xml)
      sed -i "/<effect name=\"v4a_standard_re\"/d" "$FILE"
      sed -i "/<library name=\"v4a_re\"/d" "$FILE"
      sed -i "/<libraries>/ a\        <library name=\"v4a_re\" path=\"libv4a_re.so\"\/>" "$FILE"
      sed -i "/<effects>/ a\        <effect name=\"v4a_standard_re\" library=\"v4a_re\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\"\/>" "$FILE"
      ;;
  esac
  if ! pin_viper_session0_effect "$FILE" || ! validate_viper_session0_effect "$FILE"; then
    cp -f "$BACKUP" "$FILE"
    echo "ViPER: skipped invalid session 0 patch for $FILE"
  fi
  rm -f "$BACKUP"
done

if [ -d "/odm/etc/" ]; then
  echo "Binding audio_effects.xml to odm partition..."
  mount -o bind /data/adb/modules/ViPER4Android-RE/odm/etc/audio_effects.xml /odm/etc/audio_effects.xml
fi
