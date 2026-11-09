#!/system/bin/sh
# during.sh: Wi-Fi for Linux mode. Stops Android's supplicant, runs Ubuntu's on wlan0, logs state.
R=/data/local/chroot-distro/ubuntu; BB=/data/adb/ksu/bin/busybox
sleep 2
echo "--- $(date +%T) taking over Wi-Fi"
stop wpa_supplicant
sleep 1
ip link set wlan0 up
if [ -f $R/opt/lsw/use-nm ]; then
  echo "NetworkManager mode: the payload runs wpa_supplicant (D-Bus) and NetworkManager itself"
else
$BB unshare -m sh -c "
  $BB mount --make-rprivate /
  for d in dev sys proc; do $BB mount --rbind /\$d $R/\$d; done
  exec $BB chroot $R /usr/sbin/wpa_supplicant -B -D nl80211 -i wlan0 -c /etc/lsw/wpa.conf -P /run/lsw-wpa.pid -f /var/log/lsw-wpa.log
"
echo "ubuntu wpa_supplicant started: rc=$?"
fi
i=0
while [ $i -lt 14 ]; do
  echo "--- $(date +%T)"
  ip link show wlan0 | head -1 | cut -c1-90
  ip -4 -br addr show wlan0
  ping -c1 -W1 $(ip route show table all 2>/dev/null | awk '/^default .*wlan0/{print $3; exit}') >/dev/null 2>&1 && echo "gateway ping ok" || echo "gateway ping FAIL"
  ping -c1 -W2 1.1.1.1 >/dev/null 2>&1 && echo "internet ping ok" || echo "internet ping FAIL"
  sleep 3; i=$((i+1))
done
echo "--- ubuntu wpa log tail"; tail -15 $R/var/log/lsw-wpa.log | cut -c1-150
