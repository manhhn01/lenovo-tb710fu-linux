#!/system/bin/sh
# run.sh <payload-in-rootfs> <max-seconds>: switch to Linux, run payload, always come back.
D=/data/local/tmp/lsw
R=/data/local/chroot-distro/ubuntu
BB=/data/adb/ksu/bin/busybox
PAYLOAD=$1; MAX=${2:-30}
TS=$(date +%H%M%S)
exec >>$D/run.log 2>&1
log() { echo "[$(date +%T)] run: $*"; }
log "=== $TS payload=$PAYLOAD max=${MAX}s"
[ -f $D/active ] && { log "ABORT another run active"; exit 1; }
cap=$(cat /sys/class/power_supply/battery/capacity); temp=$(( $(cat /sys/class/power_supply/battery/temp) / 10 ))
log "battery ${cap}% ${temp}C"
[ "$cap" -ge 30 ] && [ "$temp" -lt 42 ] || { log "ABORT battery/temp"; exit 1; }
[ -x "$R$PAYLOAD" ] || { log "ABORT payload missing"; exit 1; }
rm -rf $D/restore.lock; touch $D/active
# helpers first: watchdog and panic keys, each in its own session
# The session ends at the time in $DL (epoch seconds). The desktop can push it later ("extend"
# notification, written by the desktop user), so the watchdog re-reads it. Hard cap: MAX + 24 h.
# The watchdog also brings Android back when the battery is nearly empty and not charging.
DL=$R/var/lib/lsw/deadline; mkdir -p $R/var/lib/lsw; chown 1000:1000 $R/var/lib/lsw
echo $(( $(date +%s) + MAX )) > $DL; chown 1000:1000 $DL
HARD=$((MAX + 86400))
setsid sh -c "while sleep 10; do
  n=\$(date +%s); d=\$(cat $DL 2>/dev/null); case \"\$d\" in ''|*[!0-9]*) d=\$((n + 60));; esac
  [ \$n -gt \$((d + 15)) ] && { sh $D/restore.sh watchdog; exit 0; }
  c=\$(cat /sys/class/power_supply/battery/capacity); st=\$(cat /sys/class/power_supply/battery/status)
  [ \"\$c\" -le 3 ] && [ \"\$st\" != Charging ] && { sh $D/restore.sh battery; exit 0; }
done" </dev/null >/dev/null 2>&1 &
W=$!
setsid sh $D/keywatch.sh </dev/null >/dev/null 2>&1 &
K=$!
setsid sh -c "while sleep 2; do sync; done" </dev/null >/dev/null 2>&1 &
Y=$!
# "Back to Android" in KDE touches this flag (the dir is owned by the desktop user)
rm -f $R/var/lib/lsw/back-to-android
setsid sh -c "while sleep 1; do [ -f $R/var/lib/lsw/back-to-android ] && { rm -f $R/var/lib/lsw/back-to-android; setsid sh $D/restore.sh menu </dev/null >/dev/null 2>&1 & exit 0; }; done" </dev/null >/dev/null 2>&1 &
B=$!
echo "$W $K $Y $B" > $D/helpers.pid
sync
[ -f $D/during.sh ] && { setsid sh $D/during.sh > $D/run-$TS.during 2>&1 &
  echo $! >> $D/helpers.pid; }
dmesg -w > $D/run-$TS.kmsg 2>&1 &
echo $! > $D/kmsg.pid
# Bluetooth: copy the pen's pairing from Android so it keeps working on both sides (never logged)
BTC=/data/misc/bluedroid/bt_config.conf
if [ -f $BTC ] && [ -x $R/opt/lsw/import-bt-bond.py ]; then
  ( umask 077; cp $BTC $R/root/.bt_config.tmp )
  $BB chroot $R /usr/bin/env -i PATH=/usr/bin:/bin /usr/bin/python3 /opt/lsw/import-bt-bond.py /root/.bt_config.tmp 2>&1 | while read -r l; do log "bt bond: $l"; done
  rm -f $R/root/.bt_config.tmp
fi
log "stop zygote, surfaceflinger, composer"
stop zygote; stop surfaceflinger; stop vendor.qti.hardware.display.composer
sleep 1
echo "lsw-session start $TS" > /dev/kmsg
# payload in a private mount namespace chrooted into Ubuntu, killed hard at HARD (the watchdog acts first)
# every Linux process lives in its own cgroup, so restore.sh can end them all at once (cgroup.kill)
CG=/sys/fs/cgroup/lsw; mkdir -p $CG
setsid sh -c 'echo $$ > /sys/fs/cgroup/lsw/cgroup.procs; exec "$@"' sh $BB timeout -s KILL $HARD $BB unshare -m sh -c "
  $BB mount --make-rprivate /
  for d in dev sys proc; do $BB mount --rbind /\$d $R/\$d; done
  $BB mkdir -p $R/dev/shm; $BB mount -t tmpfs -o mode=1777 shm $R/dev/shm
  exec $BB chroot $R /usr/bin/env -i PATH=/usr/bin:/bin HOME=/root LANG=C.UTF-8 $PAYLOAD
" > $D/run-$TS.out 2>&1 < /dev/null
log "payload ended, exit $?"
sh $D/restore.sh normal
