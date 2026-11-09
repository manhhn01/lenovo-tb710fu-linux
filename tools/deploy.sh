#!/bin/sh
# deploy.sh [adb-serial]: copy this repo's files to a rooted TB710FU over adb and build the native tools.
# Needs: adb, root (KernelSU), the Ubuntu rootfs at /data/local/chroot-distro/ubuntu, a desktop user "user".
set -e
S=${1:-${ADB_SERIAL:?set ADB_SERIAL or pass the adb serial}}
cd "$(dirname "$0")/.."
A="adb -s $S"
R=/data/local/chroot-distro/ubuntu
T=/data/local/tmp/lsw-deploy
$A shell "rm -rf $T; mkdir -p $T"
$A push android rootfs ksu-module termux $T/ >/dev/null
$A shell "su -c '
set -e
mkdir -p /data/local/tmp/lsw $R/opt/lsw $R/var/lib/lsw
cp $T/android/*.sh /data/local/tmp/lsw/
cp -r $T/rootfs/opt/lsw/. $R/opt/lsw/ && touch $R/opt/lsw/use-nm
cp -r $T/rootfs/etc/. $R/etc/
mkdir -p $R/home/user/.local/share/applications $R/home/user/.config
cp $T/rootfs/home/user/.local/share/applications/back-to-android.desktop $R/home/user/.local/share/applications/
cp $T/rootfs/home/user/.config/startkderc $T/rootfs/home/user/.config/powerdevilrc $R/home/user/.config/
chown -R 1000:1000 $R/home/user/.local $R/home/user/.config/startkderc $R/home/user/.config/powerdevilrc $R/var/lib/lsw
mkdir -p /data/adb/modules/lsw_linux_switch && cp $T/ksu-module/lsw_linux_switch/* /data/adb/modules/lsw_linux_switch/
chmod 755 /data/adb/modules/lsw_linux_switch/action.sh $R/opt/lsw/*.sh $R/opt/lsw/*.py
chroot $R /usr/bin/env PATH=/usr/bin:/bin /bin/sh -c \"cd /opt/lsw && gcc -O2 -Wall -Wno-builtin-declaration-mismatch -shared -fPIC -o lsw-split.so lsw-split.c -I/usr/include/libdrm -ldrm -ldl -lpthread && gcc -O2 -o drmoff drmoff.c -I/usr/include/libdrm -ldrm && gcc -O2 -o absres absres.c && gcc -O2 -o lsw-inputd lsw-inputd.c && gcc -O2 -DUSE_SYSTEMD=0 -o bt-hal-bridge bt-hal-bridge.c \\\$(pkg-config --cflags --libs libgbinder libglibutil gobject-2.0 glib-2.0 bluez)\"
rm -rf $T
echo deployed
'"
echo "Termux shortcut: copy termux/.shortcuts/Linux to ~/.shortcuts/ inside Termux (needs Termux:Widget)."
