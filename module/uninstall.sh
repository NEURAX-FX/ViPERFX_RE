# Stop the init-owned daemon before the module files disappear. Missing service
# or missing stop command is not an error: the module may never have been active.
if [ -n "$(getprop init.svc.viper_daemon 2>/dev/null)" ]; then
  stop viper_daemon 2>/dev/null
fi

# Runtime/diagnostic state only. User snapshots under /data/adb/viper4android
# (current.snapshot, previous.snapshot) are deliberately preserved, and App
# Room/DataStore files are never touched.
rm -f /data/adb/viper4android/daemon.state 2>/dev/null
rm -f /data/adb/viper4android/daemon.state.tmp 2>/dev/null
rm -f /data/adb/viper4android/daemon-start.log 2>/dev/null
# The owner dex lives inside the module, so $INFO removal below handles it. This
# only clears owner runtime diagnostics that live outside the module tree.
rm -f /data/adb/viper4android/owner.log 2>/dev/null

# Don't modify anything after this
if [ -f $INFO ]; then
  while read LINE; do
    if [ "$(echo -n $LINE | tail -c 1)" == "~" ]; then
      continue
    elif [ -f "$LINE~" ]; then
      mv -f $LINE~ $LINE
    else
      rm -f $LINE
      while true; do
        LINE=$(dirname $LINE)
        [ "$(ls -A $LINE 2>/dev/null)" ] && break 1 || rm -rf $LINE
      done
    fi
  done < $INFO
  rm -f $INFO
fi
