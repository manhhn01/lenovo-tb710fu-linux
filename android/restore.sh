#!/system/bin/sh
# restore.sh <reason>: bring Android back. Idempotent: only the first caller runs.
D=/data/local/tmp/lsw
mkdir $D/restore.lock 2>/dev/null || exit 0
exec >>$D/run.log 2>&1
log() { echo "[$(date +%T)] restore: $*"; sync; }
log "triggered by $1"
# stop helpers and every test process
[ -f $D/helpers.pid ] && for p in $(cat $D/helpers.pid); do kill -9 $p 2>/dev/null; done
pkill -9 -f /opt/lsw/ 2>/dev/null
R=/data/local/chroot-distro/ubuntu
[ -f $R/run/lsw-wpa.pid ] && { kill $(cat $R/run/lsw-wpa.pid) 2>/dev/null; rm -f $R/run/lsw-wpa.pid; log "stopped ubuntu wpa_supplicant"; sleep 1; }
for n in cage glmark2-es2-wayland vkcube foot kwin_wayland sway swaybar modetest kmscube Xwayland; do pkill -9 -x $n 2>/dev/null; done
# fast path: the payload and everything it started sit in one cgroup (run.sh)
CG=/sys/fs/cgroup/lsw; slow=1
if [ -f $CG/cgroup.kill ]; then
  n=$(wc -l < $CG/cgroup.procs); [ "$n" -gt 0 ] && echo 1 > $CG/cgroup.kill
  i=0; while [ -n "$(cat $CG/cgroup.procs)" ] && [ $i -lt 25 ]; do sleep 0.2; i=$((i+1)); done
  left=$(wc -l < $CG/cgroup.procs); log "cgroup kill: $n processes, $left left"; [ "$left" = 0 ] && slow=
fi
[ -n "$slow" ] && sh $D/killstray.sh 2>&1 | while read -r l; do log "$l"; done
# anything still holding the display (e.g. KDE inside the container) must let go before Android starts
ip rule del pref 50 2>/dev/null; ip -6 rule del pref 50 2>/dev/null   # Linux-mode routing rule (lsw-nm.sh)
# Linux mode inside the container (p59): its systemd runs NetworkManager; stop it before Android takes Wi-Fi back
CP=$(cat $R/tmp/lsw-cpid 2>/dev/null)
[ -n "$CP" ] && [ "$(cat /proc/$CP/comm 2>/dev/null)" = systemd ] && /data/adb/ksu/bin/busybox timeout 10 /data/adb/ksu/bin/busybox nsenter -t $CP -m -p -- /usr/bin/systemctl stop NetworkManager wpa_supplicant 2>/dev/null
log "step: kill display holders"
for n in plasmashell kwin_wayland kwin_wayland_wr startplasma-way plasma_session ksmserver kded6 ksplashqml; do pkill -9 -x $n 2>/dev/null; done
[ -n "$slow" ] && for p in $(ls /proc | grep -E '^[0-9]+$'); do ls -l /proc/$p/fd 2>/dev/null | grep -q 'dri/card0' && { kill -9 $p 2>/dev/null && log "killed card0 holder $p $(cat /proc/$p/comm 2>/dev/null)"; }; done
log "step: drmoff"
echo 0 > /sys/module/drm/parameters/debug
[ -f $D/kmsg.pid ] && kill $(cat $D/kmsg.pid) 2>/dev/null
sleep 1
# leave the display clean even if the compositor died without cleanup
BB=/data/adb/ksu/bin/busybox
$BB timeout -s KILL 8 $BB unshare -m sh -c "
  $BB mount --make-rprivate /
  for d in dev sys proc; do $BB mount --rbind /\$d $R/\$d; done
  exec $BB chroot $R /opt/lsw/drmoff" 2>&1 | while read -r l; do log "drmoff: $l"; done
log "start composer, surfaceflinger, zygote"
start vendor.qti.hardware.display.composer; sleep 2
start surfaceflinger; sleep 2
start zygote
[ "$(getprop init.svc.wpa_supplicant)" = running ] || { start wpa_supplicant; log "started android wpa_supplicant"; }
i=0
while [ $i -lt 90 ]; do
  if pidof system_server >/dev/null && service check activity | grep -q found; then
    log "system_server back after ${i}s"
    # health check: SurfaceFlinger must answer, or the UI is frozen
    sleep 12
    if timeout 10 dumpsys SurfaceFlinger --displays >/dev/null 2>&1 && [ "$(getprop init.svc.vendor.qti.hardware.display.composer)" = running ]; then
      log "health ok: SurfaceFlinger answers, composer running"; rm -f $D/active; exit 0
    fi
    log "FAILSAFE: SurfaceFlinger not answering or composer down, rebooting"
    sync; setprop sys.powerctl reboot,lsw-ui-frozen; sleep 20; /data/adb/ksu/bin/busybox reboot -f
  fi
  sleep 1; i=$((i+1))
done
log "FAILSAFE: Android not back in 90s, rebooting"
sync; setprop sys.powerctl reboot,lsw-failsafe; sleep 20; /data/adb/ksu/bin/busybox reboot -f
