echo -n $LIBPATCH > $MODPATH/libpatch.txt

ui_print "    Copying lib files..."

cp_ch -n $MODPATH/common/files/libv4a_re_$ABI32.so $MODPATH$LIBDIR/lib/soundfx/libv4a_re.so
if [ "$IS64BIT" ]; then
cp_ch -n $MODPATH/common/files/libv4a_re_$ABI.so $MODPATH$LIBDIR/lib64/soundfx/libv4a_re.so
fi

ui_print "    Installing root audio daemon..."

# functions.sh cleanup() deletes $MODPATH/common after install, so the daemon
# binary must be copied to a persistent location inside the module.
cp_ch -n $MODPATH/common/bin/viper-daemon_$ABI $MODPATH/bin/viper-daemon 0755

# The ART effect owner. Root-readable only: app_process runs it as root, and the
# dex is not something an app should be able to read or replace.
#
# A missing dex is not fatal. The init service passes --owner-dex unconditionally
# and the daemon treats an unreadable path as a spawn failure: after a bounded
# number of attempts it reports owner_state=failed and the App's legacy backend
# keeps applying state. Aborting the install would leave a working setup unusable.
if [ -f $MODPATH/common/bin/viper-owner.dex ]; then
  cp_ch -n $MODPATH/common/bin/viper-owner.dex $MODPATH/bin/viper-owner.dex 0644
else
  ui_print "    ! Owner dex missing; App-owned effect fallback stays in charge"
fi

# KernelSU injects every initrc/*.rc into init.rc; Magisk ignores the directory.
if [ -f $MODPATH/initrc/viper-daemon.rc ]; then
  set_perm $MODPATH/initrc/viper-daemon.rc 0 0 0644
fi
if [ "$KSU" != "true" ]; then
  ui_print "    ! No initrc injection on this root solution"
  ui_print "    ! Daemon stays inactive; App-to-driver backend keeps working"
fi

ui_print "    Patching audio_effects config files"
CFGS="$(find /odm /system /vendor -type f -name "*audio_effects*.conf" -o -name "*audio_effects*.xml")"
for OFILE in ${CFGS}; do
  FILE="$MODPATH$(echo $OFILE | sed "s|^/vendor|/system/vendor|g")"
  cp_ch -n $OFILE $FILE
  case $FILE in
    *.conf)
        sed -i "/v4a_standard_re {/,/}/d" $FILE
        sed -i "/v4a_re {/,/}/d" $FILE
        sed -i "s/^effects {/effects {\n  v4a_standard_re {\n    library v4a_re\n    uuid 90380da3-8536-4744-a6a3-5731970e640f\n  }/g" $FILE
        sed -i "s/^libraries {/libraries {\n  v4a_re {\n    path $LIBPATCH\/lib\/soundfx\/libv4a_re.so\n  }/g" $FILE
        ;;
    *.xml)
        sed -i "/v4a_standard_re/d" $FILE
        sed -i "/v4a_re/d" $FILE
        sed -i "/<libraries>/ a\        <library name=\"v4a_re\" path=\"libv4a_re.so\"\/>" $FILE
        sed -i "/<effects>/ a\        <effect name=\"v4a_standard_re\" library=\"v4a_re\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\"\/>" $FILE
        ;;
  esac
done