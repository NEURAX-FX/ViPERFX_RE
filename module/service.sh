MODDIR=${0%/*}
# late_start service stage. KernelSU also has a boot-completed stage, but Magisk
# does not, so this hook waits for the boot property itself. Both paths call the
# same idempotent helper, so running on KernelSU twice is harmless.
until [ "$(getprop sys.boot_completed)" = "1" ]; do
  sleep 1
done
sh $MODDIR/daemon-start.sh
