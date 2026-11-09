#!/system/bin/sh
# keywatch.sh [test]: Vol Up + Vol Down pressed together -> restore (or just log in test mode)
D=/data/local/tmp/lsw
up=0; dn=0
getevent -ql 2>/dev/null | while read -r dev type code val rest; do
  [ "$type" = EV_KEY ] || continue
  case "$code" in
    KEY_VOLUMEUP)   [ "$val" = DOWN ] && up=1 || { [ "$val" = UP ] && up=0; } ;;
    KEY_VOLUMEDOWN) [ "$val" = DOWN ] && dn=1 || { [ "$val" = UP ] && dn=0; } ;;
    *) continue ;;
  esac
  if [ $up = 1 ] && [ $dn = 1 ]; then
    if [ "$1" = test ]; then echo "[$(date +%T)] keywatch: BOTH VOLUME KEYS (test mode)" >> $D/run.log; up=0; dn=0
    else sh $D/restore.sh panic-keys; exit 0; fi
  fi
done
