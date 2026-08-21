MODDIR=${0%/*}
# KernelSU runs this on boot completed. The rc file also starts the service on
# property:sys.boot_completed=1; both paths are idempotent because init owns the
# service, so whichever fires first wins and the other is a no-op.
sh $MODDIR/daemon-start.sh
