#!/system/bin/sh
# killstray.sh: kill processes started from the Ubuntu rootfs by our chroot tests (they live in
# Android's PID namespace). Container processes (their own PID namespace) are left alone.
R=/data/local/chroot-distro/ubuntu
HOSTNS=$(readlink /proc/1/ns/pid)
n=0
for p in /proc/[0-9]*; do
  pid=${p#/proc/}
  [ "$(readlink $p/ns/pid 2>/dev/null)" = "$HOSTNS" ] || continue
  exe=$(readlink $p/exe 2>/dev/null)
  root=$(readlink $p/root 2>/dev/null)
  case "$exe" in
    $R/*) ;;
    /opt/hypr/*|/opt/mesa-zink/*|/usr/bin/*|/usr/libexec/*|/usr/lib/*|/usr/sbin/*) [ "$root" = "$R" ] || continue ;;
    *) continue ;;
  esac
  kill -9 $pid 2>/dev/null && n=$((n+1)) && echo "killed $pid $(cat $p/comm 2>/dev/null) ($exe)"
done
echo "killstray: $n processes"
