#!/usr/bin/python3
# import-bt-bond.py <bt_config.conf> [name-substring ...]: copy Bluetooth pairings from Android to
# BlueZ, so a device keeps working on both sides without pairing again (same controller address).
# With no names, every device paired on Android is copied (names otherwise matched ignoring case).
# Handles LE and classic pairings. Writes /var/lib/bluetooth/<adapter>/<device>/info, mode 600.
# Never prints a key or an address. Runs before each Linux session, so new pairings carry over.
import os, sys

src = sys.argv[1]
wanted = [w.lower() for w in sys.argv[2:]]
sections, cur = {}, None
for line in open(src, errors="replace"):
    line = line.strip()
    if line.startswith("[") and line.endswith("]"):
        cur = line[1:-1]; sections[cur] = {}
    elif "=" in line and cur:
        k, v = line.split("=", 1); sections[cur][k.strip()] = v.strip()
adapter = sections.get("Adapter", {}).get("Address", "").upper()
if not adapter:
    sys.exit("no adapter address")
base = "/var/lib/bluetooth"
count = 0
for section, d in sections.items():
    name = d.get("Name", "")
    if wanted and not any(w in name.lower() for w in wanted):
        continue
    if "LE_KEY_PENC" not in d and "LinkKey" not in d:        # not paired, or not a device section
        continue
    addr = section.upper()
    tech = ["BR/EDR"] if "LinkKey" in d else []
    tech += ["LE"] if "LE_KEY_PENC" in d else []
    out = ["[General]", "Name=" + name, "SupportedTechnologies=" + ";".join(tech) + ";", "Trusted=true", "Blocked=false"]
    if "DevClass" in d and "LinkKey" in d:
        out.append("Class=0x%06x" % int(d["DevClass"]))
    if "LE_KEY_PENC" in d:
        out.append("AddressType=" + ("static" if d.get("AddrType", "1") == "1" else "public"))
    out.append("")
    if "LinkKey" in d:
        key = bytes.fromhex(d["LinkKey"])[::-1]     # Android stores it in the opposite byte order to BlueZ
        out += ["[LinkKey]", "Key=" + key.hex().upper(), "Type=%d" % int(d.get("LinkKeyType", "4")),
                "PINLength=%d" % int(d.get("PinLength", "0")), ""]
    if "LE_KEY_PENC" in d:
        penc = bytes.fromhex(d["LE_KEY_PENC"])      # ltk[16] rand[8] ediv[2] sec_level[1] key_size[1]
        out += ["[LongTermKey]", "Key=" + penc[:16].hex().upper(), "Authenticated=0", "EncSize=%d" % (penc[27] or 16),
                "EDiv=%d" % int.from_bytes(penc[24:26], "little"), "Rand=%d" % int.from_bytes(penc[16:24], "little"), ""]
        if "LE_KEY_PID" in d:                       # irk[16] addr_type[1] addr[6]
            out += ["[IdentityResolvingKey]", "Key=" + bytes.fromhex(d["LE_KEY_PID"])[:16].hex().upper(), ""]
    os.makedirs(base + "/%s/%s" % (adapter, addr), mode=0o700, exist_ok=True)
    os.chmod(base, 0o700)
    path = base + "/%s/%s/info" % (adapter, addr)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.write(fd, ("\n".join(out) + "\n").encode())
    os.close(fd)
    count += 1
    print("imported:", name, "(" + ", ".join(tech) + ")")
print("imported", count, "device(s)")
