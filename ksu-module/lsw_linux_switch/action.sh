#!/system/bin/sh
# KernelSU Action: start a Linux (KDE) session. The switch stops zygote, which ends the KernelSU
# app and its process group, so leave the app's cgroup and session before starting run.sh.
D=/data/local/tmp/lsw
if [ -f $D/active ]; then echo "A Linux session is already running."; exit 0; fi
cap=$(cat /sys/class/power_supply/battery/capacity)
[ "$cap" -ge 30 ] || { echo "Battery at ${cap}%: needs 30% or more."; exit 1; }
echo "Switching to Linux in 3 s. Back to Android: \"Back to Android\" on the KDE desktop, or Vol Up + Vol Down."
# the session runs inside the Droidspaces container (systemd, logind, D-Bus): start it if needed
R=/data/local/chroot-distro/ubuntu; DS=/data/local/Droidspaces/bin/droidspaces
P=$($DS -n ubuntu pid 2>/dev/null | tr -dc 0-9)
if [ -z "$P" ] || [ "$(cat /proc/$P/comm 2>/dev/null)" != systemd ]; then
  echo "Starting the Ubuntu container..."
  $DS -C /data/local/Droidspaces/Containers/ubuntu/container.config start >/dev/null 2>&1
  i=0; while [ $i -lt 30 ]; do P=$($DS -n ubuntu pid 2>/dev/null | tr -dc 0-9); [ -n "$P" ] && break; sleep 1; i=$((i+1)); done
  sleep 5
fi
[ -n "$P" ] || { echo "The container did not start."; exit 1; }
echo $P > $R/tmp/lsw-cpid
echo $$ > /sys/fs/cgroup/cgroup.procs 2>/dev/null
( sleep 3; exec setsid sh $D/run.sh /opt/lsw/p64.sh 14400 ) </dev/null >/dev/null 2>&1 &
exit 0
