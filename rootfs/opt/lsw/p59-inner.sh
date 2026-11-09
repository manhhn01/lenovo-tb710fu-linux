#!/bin/sh
# payload 59 (inside the container, root): devices, NetworkManager/UPower via systemd, then KDE as user.
U=user; UID_=1000; GID_=1000; H=/home/user
# Android mounts the rootfs nosuid. Without suid, unix_chkpwd cannot read /etc/shadow, so the KDE
# lock screen rejects every password, and sudo does not work. Applies to this container only.
mount -o remount,suid / 2>/dev/null
mkdir -p /dev/dri /dev/input
[ -e /dev/dri/card0 ] || mknod -m 666 /dev/dri/card0 c 226 0
[ -e /dev/dri/renderD128 ] || mknod -m 666 /dev/dri/renderD128 c 226 128
for d in /sys/class/input/event*; do n=${d##*event}; mm=$(cat $d/dev); rm -f /dev/input/event$n; mknod -m 660 /dev/input/event$n c ${mm%%:*} ${mm##*:}; chown $UID_:$GID_ /dev/input/event$n; done
# UPower (root, but without DAC override) reads the cover switch: it must own that node
for d in /sys/class/input/event*; do [ "$(cat $d/device/name)" = hall_irq ] && chown 0:$GID_ /dev/input/${d##*/}; done
# pen: NVTCapacitivePen reports resolution 0 (libinput then ignores it). Axes are 0..20000 x
# 0..32000 for the 2000 x 3200 px panel, about 100 units per mm on a 14.5-inch 16:10 screen.
for d in /sys/class/input/event*; do case "$(cat $d/device/name)" in *Pen*) /opt/lsw/absres /dev/input/${d##*/} 100 >/dev/null;; esac; done
/opt/lsw/lsw-inputd --init
# input hotplug (keyboard cover reconnected): libudev subscribes to device events only if this file
# exists when KWin starts, otherwise KWin never hears about new devices. lsw-inputd sends them.
touch /run/udev/control
/opt/lsw/lsw-inputd > /tmp/lsw-inputd.out 2>&1 &
HP=$!
# udisks subscribes to device events when it starts, so restart it now that they are available
systemctl restart udisks2 2>/dev/null
idx=$(cat /sys/class/net/wlan0/ifindex); printf 'I:1\nE:ID_NET_DRIVER=kiwi\n' > /run/udev/data/n$idx
for d in /sys/class/power_supply/*; do printf 'I:1\n' > "/run/udev/data/+power_supply:${d##*/}"; done
# brightness: PowerDevil's root helper writes /sys/class/backlight/panel0-backlight (raw 0..4095),
# so the container's /sys must be writable for the session (made read-only again at the end)
mount -o remount,rw /sys 2>/dev/null
for d in /sys/class/backlight/*; do printf 'I:1\n' > "/run/udev/data/+backlight:${d##*/}"; done
# Wi-Fi: NetworkManager + wpa_supplicant from the container's systemd, wlan0 only, no DNS changes
mkdir -p /run/NetworkManager/conf.d
printf '[main]\ndns=none\n[keyfile]\nunmanaged-devices=except:interface-name:wlan0\n[connectivity]\nenabled=false\n[device]\nwifi.scan-rand-mac-address=no\n' > /run/NetworkManager/conf.d/90-lsw.conf
ip rule add pref 50 lookup main 2>/dev/null; ip -6 rule add pref 50 lookup main 2>/dev/null
systemctl unmask wpa_supplicant NetworkManager >/dev/null 2>&1
systemctl start wpa_supplicant NetworkManager
systemctl restart upower      # it reads input devices only when it starts
echo "services: $(systemctl is-active wpa_supplicant NetworkManager upower systemd-logind polkit | tr '\n' ' ')"
# Bluetooth (LSW_BT): hci0 comes from bt-hal-bridge; BlueZ runs here only during the session
if [ -n "$LSW_BT" ]; then
  for n in uhid rfkill; do [ -e /dev/$n ] || { mm=$(cat /sys/class/misc/$n/dev); mknod -m 660 /dev/$n c ${mm%%:*} ${mm##*:}; }; done
  chown $UID_ /dev/rfkill       # KDE's Bluetooth settings read it as the desktop user
  for i in $(seq 40); do [ -e /sys/class/bluetooth/hci0 ] && break; sleep 0.5; done
  systemctl unmask bluetooth >/dev/null 2>&1; systemctl start bluetooth
  echo "bluetooth: hci0 $([ -e /sys/class/bluetooth/hci0 ] && echo present || echo missing), service $(systemctl is-active bluetooth)"
fi
# Lotus (Vietnamese input) uses a small root service and /dev/uinput for its Uinput modes
[ -e /dev/uinput ] || { mm=$(cat /sys/class/misc/uinput/dev); mknod -m 600 /dev/uinput c ${mm%%:*} ${mm##*:}; }
systemctl start fcitx5-lotus-server@$U 2>/dev/null
mkdir -p /run/user/$UID_; chown $UID_:$GID_ /run/user/$UID_; chmod 700 /run/user/$UID_
mkdir -p /tmp/.X11-unix; chmod 1777 /tmp /tmp/.X11-unix
rm -f /tmp/lsw-fb.ppm
# KWin's saved display setup (scale, rotation) is kept between sessions. A hard hang can leave it
# half written, so drop it only if it is not valid JSON. With no saved setup, set scale 2 below.
C=$H/.config/kwinoutputconfig.json
[ -f $C ] && ! python3 -c 'import json,sys; json.load(open(sys.argv[1]))' $C 2>/dev/null && { echo "dropping corrupt $C"; rm -f $C; }
[ -f $C ] && HAVE_DISPLAY_CFG=1
O=/tmp/plasma-59.out
# rendering: LSW_GPU=1 -> OpenGL through Zink on Turnip (KGSL); otherwise CPU only
Z=/opt/mesa-zink/lib/aarch64-linux-gnu
if [ "$LSW_GPU" = 3 ]; then   # all clients on the GPU through the glvnd build of Mesa Zink
  G=/opt/mesa-zink-glvnd
  REND="LD_LIBRARY_PATH=$G/lib/aarch64-linux-gnu LIBGL_DRIVERS_PATH=$G/lib/aarch64-linux-gnu/dri GBM_BACKENDS_PATH=$G/lib/aarch64-linux-gnu/gbm __EGL_VENDOR_LIBRARY_FILENAMES=$G/share/glvnd/egl_vendor.d/50_mesa.json VK_ICD_FILENAMES=$G/share/vulkan/icd.d/freedreno_icd.aarch64.json MESA_LOADER_DRIVER_OVERRIDE=zink TU_DEBUG=noconform KWIN_NO_TIMER_QUERY=1 XWAYLAND_NO_GLAMOR=1"
elif [ "$LSW_GPU" = 2 ]; then   # only KWin on the GPU (set up by the shim inside kwin_wayland_wrapper)
  REND="LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe QT_QUICK_BACKEND=software LSW_GPU_KWIN=1 KWIN_NO_TIMER_QUERY=1 XWAYLAND_NO_GLAMOR=1"
elif [ "$LSW_GPU" = 1 ]; then
  REND="LD_LIBRARY_PATH=$Z LIBGL_DRIVERS_PATH=$Z/dri GBM_BACKENDS_PATH=$Z/gbm VK_ICD_FILENAMES=/opt/mesa-zink/share/vulkan/icd.d/freedreno_icd.aarch64.json MESA_LOADER_DRIVER_OVERRIDE=zink TU_DEBUG=noconform KWIN_NO_TIMER_QUERY=1 XWAYLAND_NO_GLAMOR=1"   # glamor on Zink stalls Xwayland (and so ksmserver)
else
  REND="LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe QT_QUICK_BACKEND=software KWIN_COMPOSE=Q"
fi
echo "rendering mode: ${LSW_GPU:-0} (0 cpu, 1 gpu everywhere, 2 gpu kwin only)"
[ -n "$HAVE_DISPLAY_CFG" ] || ( sleep 15; setpriv --reuid=$UID_ --regid=$GID_ --init-groups env -i PATH=/usr/bin:/bin HOME=$H XDG_RUNTIME_DIR=/run/user/$UID_ WAYLAND_DISPLAY=wayland-0 sh -c 'out=$(kscreen-doctor -o 2>&1 | sed "s/\x1b\[[0-9;]*m//g" | awk "/^Output:/{print \$3; exit}"); kscreen-doctor output.$out.scale.2; echo "scale set on $out"' > /tmp/kscreen.out 2>&1 ) &
# audio server: PipeWire + Pulse + WirePlumber as the desktop user once Plasma's session bus is up
# (no sound card yet: a null sink from /etc/pipewire/pipewire.conf.d/50-lsw-null.conf; Pulse
# clients such as Xournal++ abort when no server answers)
for n in wireplumber pipewire-pulse pipewire; do for p in $(pgrep -x $n); do kill $p; done; done   # left over from an earlier session
( for i in $(seq 60); do P=$(pgrep -x plasmashell | head -1); [ -n "$P" ] && break; sleep 2; done
  A=$(tr '\0' '\n' < /proc/$P/environ | grep ^DBUS_SESSION_BUS_ADDRESS=)
  for b in pipewire wireplumber pipewire-pulse; do
    setpriv --reuid=$UID_ --regid=$GID_ --init-groups env -i PATH=/usr/bin:/bin HOME=$H XDG_RUNTIME_DIR=/run/user/$UID_ "$A" $b > /tmp/$b.out 2>&1 &
    sleep 1
  done ) &
# notifications: session deadline (extend buttons) and low battery
/opt/lsw/lsw-notify.sh > /tmp/lsw-notify.out 2>&1 &
NP=$!
cd $H   # apps started from the desktop inherit this as their working folder
setsid -w sh -c 'echo $$ > /tmp/lsw-sid; exec "$@"' sh timeout -s KILL ${LSW_SECS:-1800} \
  setpriv --reuid=$UID_ --regid=$GID_ --init-groups env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin HOME=$H USER=$U LOGNAME=$U SHELL=/bin/sh LANG=C.UTF-8 \
  XDG_RUNTIME_DIR=/run/user/$UID_ XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=KDE KWIN_DRM_DEVICES=/dev/dri/card0 XMODIFIERS=@im=fcitx "QT_IM_MODULES=wayland;fcitx" \
  $REND QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough POWERDEVIL_NO_DDCUTIL=1 \
  LSW_KWIN_NO_SYSBUS=1 LSW_STRIPS=155,151,147,143 LSW_NO_GBM_HOOKS=1 LSW_FORCE_PREFERRED=1 LSW_STATS=1 $LSW_EXTRA LD_PRELOAD=/opt/lsw/lsw-split.so \
  dbus-run-session -- startplasma-wayland > $O 2>&1
echo "session exit $?"
pkill -9 -s "$(cat /tmp/lsw-sid)" 2>/dev/null
kill $NP $HP 2>/dev/null
systemctl stop NetworkManager wpa_supplicant upower
systemctl stop fcitx5-lotus-server@$U 2>/dev/null
[ -n "$LSW_BT" ] && { systemctl stop bluetooth; systemctl mask bluetooth >/dev/null 2>&1; }
ip rule del pref 50 2>/dev/null; ip -6 rule del pref 50 2>/dev/null
mount -o remount,ro /sys 2>/dev/null
cat /tmp/kscreen.out 2>/dev/null | head -2
echo "fps: $(grep '^lsw-fps' $O | awk '{print $3}' | head -20 | tr '\n' ' ')"
grep -oE "^(kwin_libinput|kde.plasmashell|ksmserver)" $O | sort | uniq -c
grep -iE "opengl|egl|zink|turnip|gbm|vulkan|renderer" $O | grep -v lsw- | sort | uniq -c | sort -rn | head -10 | cut -c1-200
