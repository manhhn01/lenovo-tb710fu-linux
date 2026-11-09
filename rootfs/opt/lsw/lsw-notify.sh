#!/bin/sh
# lsw-notify.sh: runs as root inside the container during a session. Desktop notifications for
#  - the session deadline (/var/lib/lsw/deadline, read by the Android-side watchdog):
#    10 min and 2 min before it ends, with buttons that push it back by 1 h / 4 h
#  - low battery when not charging: warnings at 15 % and 8 %, back to Android at 5 %
DL=/var/lib/lsw/deadline; FLAG=/var/lib/lsw/back-to-android
BAT=/sys/class/power_supply/battery
note() {   # note <urgency> <title> <body> [actions...]: as the desktop user, prints the chosen action
  P=$(pgrep -x plasmashell | head -1); [ -n "$P" ] || return
  A=$(tr '\0' '\n' < /proc/$P/environ | grep ^DBUS_SESSION_BUS_ADDRESS=)
  u=$1 t=$2 b=$3; shift 3
  setpriv --reuid=1000 --regid=1000 --init-groups env -i PATH=/usr/bin:/bin XDG_RUNTIME_DIR=/run/user/1000 "$A" \
    notify-send -a "Linux mode" -i preferences-system-time -u "$u" "$@" "$t" "$b" 2>/dev/null
}
extend_ui() {   # runs in the background; waits for a button press
  r=$(note critical "$1" "Linux mode ends at $(date -d @$(cat $DL) +%H:%M). Save your work, or extend." \
        -A h1="Extend 1 hour" -A h4="Extend 4 hours" -A now="Back to Android now" --wait)
  d=$(cat $DL)
  case "$r" in
    h1) echo $((d + 3600)) > $DL ;;
    h4) echo $((d + 14400)) > $DL ;;
    now) touch $FLAG ;;
  esac
  [ -n "$r" ] && [ "$r" != now ] && note normal "Session extended" "Linux mode now ends at $(date -d @$(cat $DL) +%H:%M)."
}
sleep 30   # let Plasma come up
w10= w2= b15= b8=
while sleep 15; do
  n=$(date +%s); d=$(cat $DL 2>/dev/null); case "$d" in ''|*[!0-9]*) d=;; esac
  if [ -n "$d" ]; then
    left=$((d - n))
    [ "$left" -gt 600 ] && w10= && w2=   # extended: arm the warnings again
    [ "$left" -le 600 ] && [ "$left" -gt 120 ] && [ -z "$w10" ] && { w10=1; extend_ui "Linux mode ends in 10 minutes" & }
    [ "$left" -le 120 ] && [ -z "$w2" ] && { w2=1; w10=1; extend_ui "Linux mode ends in 2 minutes" & }
  fi
  c=$(cat $BAT/capacity); st=$(cat $BAT/status)
  if [ "$st" = Charging ] || [ "$st" = Full ]; then b15= b8=; continue; fi
  [ "$c" -le 15 ] && [ -z "$b15" ] && { b15=1; note normal "Battery low: $c %" "Plug in the charger. Linux mode returns to Android at 5 %." & }
  [ "$c" -le 8 ] && [ -z "$b8" ] && { b8=1; note critical "Battery very low: $c %" "Save your work. Back to Android at 5 %." & }
  [ "$c" -le 5 ] && { note critical "Battery at $c %" "Returning to Android now."; sleep 5; touch $FLAG; exit 0; }
done
