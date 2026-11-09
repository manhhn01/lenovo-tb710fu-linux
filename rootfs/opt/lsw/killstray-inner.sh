#!/bin/sh
# inside the chroot: kill leftover test processes between phases. Only processes in our own PID
# namespace (Android's), never the container's (XFCE, its dbus), and never this script or its parents.
me=$(readlink /proc/self/ns/pid)
for p in /proc/[0-9]*; do
  pid=${p#/proc/}
  [ "$pid" = $$ ] && continue
  [ "$(readlink $p/ns/pid 2>/dev/null)" = "$me" ] || continue
  case "$(cat $p/comm 2>/dev/null)" in
    plasmashell|kwin_wayland|kwin_wayland_wr|ksmserver|kded6|Xwayland|maliit-keyboar|kactivitymanag|polkit-kde-auth|xdg-desktop-por|baloo_file|dbus-daemon|dbus-run-sessio|startplasma-way|plasma_session|kglobalaccel|konsole|dolphin)
      kill -9 $pid 2>/dev/null ;;
  esac
done
true
