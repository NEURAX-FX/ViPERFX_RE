MODDIR=${0%/*}
# Asks init to start the daemon. init owns the process lifetime; this script
# never execs the daemon itself, so running it twice cannot create a second
# instance.
#
# Requires the service definition in initrc/viper-daemon.rc to have reached
# init. KernelSU injects it while init reads init.rc. Magisk has no such
# injection point, and KernelSU late-load mode skips the read() hook, so there
# the service is simply absent and we report that instead of faking it.
STATE_DIR=/data/adb/viper4android
MARKER=$STATE_DIR/daemon-start.log

mkdir -p $STATE_DIR
chmod 0700 $STATE_DIR

log_marker() {
  echo "$(date 2>/dev/null) $1" >> $MARKER
}

SVC_STATE=$(getprop init.svc.viper_daemon)
if [ -z "$SVC_STATE" ]; then
  log_marker "unsupported: init has no viper_daemon service; keeping legacy App-to-driver backend"
  exit 0
fi

if [ ! -x $MODDIR/bin/viper-daemon ]; then
  log_marker "error: $MODDIR/bin/viper-daemon missing or not executable"
  exit 1
fi

if [ "$SVC_STATE" = "running" ]; then
  log_marker "already running (init.svc.viper_daemon=running)"
  exit 0
fi

start viper_daemon
log_marker "requested start; init.svc.viper_daemon=$(getprop init.svc.viper_daemon)"
