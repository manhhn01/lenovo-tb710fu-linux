#!/bin/sh
# payload 59 (chroot side): run the KDE session inside the Droidspaces container (systemd, logind,
# system D-Bus, polkit). The container must be running; its init PID is in /tmp/lsw-cpid.
P=$(cat /tmp/lsw-cpid 2>/dev/null)
[ -n "$P" ] && [ "$(cat /proc/$P/comm 2>/dev/null)" = systemd ] || { echo "container not running"; exit 1; }
echo "entering container init $P"
# Bluetooth: the bridge needs Android's real /dev/hwbinder, which only this (chroot) side can see
# It exits if Android's service drops the link (turning Bluetooth off in KDE can do that), so keep it running.
[ -n "$LSW_BT" ] && { ( while :; do /opt/lsw/bt-hal-bridge; echo "bridge exited $?"; sleep 3; done ) > /tmp/bt-bridge.out 2>&1 & echo "bluetooth bridge $!"; }
exec nsenter -t $P -m -u -i -p -- /usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin LANG=C.UTF-8 LSW_SECS=${LSW_SECS:-172800} LSW_GPU=$LSW_GPU LSW_EXTRA="$LSW_EXTRA" LSW_BT=$LSW_BT /opt/lsw/p59-inner.sh
