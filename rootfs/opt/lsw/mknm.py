#!/usr/bin/python3 -I
# Import Android's saved Wi-Fi networks (WifiConfigStore.xml, argv[1]) as NetworkManager keyfiles
# /etc/NetworkManager/system-connections/lsw-*.nmconnection (mode 600). Prints only counts, never keys.
import sys, os, re, glob, uuid, xml.etree.ElementTree as ET
raw = open(sys.argv[1], encoding="utf-8", errors="replace").read()
raw = re.sub(r"&#(\d+);", lambda m: m.group(0) if int(m.group(1)) >= 32 or int(m.group(1)) in (9, 10, 13) else "?", raw)
root = ET.fromstring(raw)
d = "/etc/NetworkManager/system-connections"
os.makedirs(d, exist_ok=True)
for f in glob.glob(d + "/lsw-*.nmconnection"): os.remove(f)
n = skipped = 0
for net in root.iter("Network"):
    wc = net.find("WifiConfiguration")
    if wc is None: continue
    v = {e.get("name"): (e.text or "") for e in wc if e.get("name")}
    ssid, psk, key = v.get("SSID", ""), v.get("PreSharedKey", ""), v.get("ConfigKey", "")
    if not (ssid.startswith('"') and ssid.endswith('"')): skipped += 1; continue
    s = ssid[1:-1]
    if not 1 <= len(s.encode()) <= 32 or not s.isprintable() or "\n" in s: skipped += 1; continue
    lines = ["[connection]", "id=" + s, "uuid=" + str(uuid.uuid5(uuid.NAMESPACE_URL, "lsw-wifi:" + s)), "type=wifi", "autoconnect=true", "",
             "[wifi]", "mode=infrastructure", "ssid=" + s, ""]
    if key.endswith("WPA_PSK"):
        p = psk[1:-1] if psk.startswith('"') else psk
        if not (8 <= len(p) <= 63 or (len(p) == 64 and all(c in "0123456789abcdefABCDEF" for c in p))): skipped += 1; continue
        lines += ["[wifi-security]", "key-mgmt=wpa-psk", "psk=" + p, ""]
    elif not key.endswith("NONE"):
        skipped += 1; continue
    lines += ["[ipv4]", "method=auto", "", "[ipv6]", "method=auto", ""]
    fn = os.path.join(d, "lsw-%02d.nmconnection" % n)
    fd = os.open(fn, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.write(fd, "\n".join(lines).encode()); os.close(fd); n += 1
print(f"NetworkManager connections written: {n}, skipped: {skipped}")
