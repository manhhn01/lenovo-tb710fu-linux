# How it works, and what was learned

Hard-won facts. Most of them cost hangs and reboots to find.

## Switch sequence

1. `run.sh` checks battery (≥ 30 %) and temperature, starts helpers in their own sessions: watchdog, panic-key watcher (`keywatch.sh`), a `sync` loop, the "Back to Android" flag watcher, and `during.sh`.
2. Stops `zygote`, `surfaceflinger` and the display composer (`vendor.qti.hardware.display.composer`).
3. Runs the payload chrooted into Ubuntu in a private mount namespace, inside cgroup `/sys/fs/cgroup/lsw`, under `timeout -s KILL`.
4. `p59.sh` enters the Droidspaces container (`nsenter -t <systemd pid> -m -u -i -p`) and runs `p59-inner.sh`, which prepares devices and starts `startplasma-wayland` as the desktop user.
5. `restore.sh` (normal end, panic keys, flag, watchdog): `cgroup.kill` ends every Linux process at once, `drmoff` if needed, removes network tweaks, restarts the composer, SurfaceFlinger and zygote, and checks health. Takes ~15 s.

## Display (Qualcomm SDE / msm_drm)

- The panel is 3200×2000, driven through SDE "layout split": a plane must not cross the split. The compositor only knows one plane, so the LD_PRELOAD shim `lsw-split.so` rewrites each atomic commit.
- The 2×1600 split (planes 127 + 131) gives rainbow noise. What works: **4 strips of 800 px** on planes **155, 151, 147, 143** (`LSW_STRIPS`).
- With 4 planes, plain page flips hang the SoC. Every commit must be **TWRP style**: full state (CRTC `MODE_ID`, `ACTIVE=1`, connector `CRTC_ID`), `ALLOW_MODESET`, blocking (no `NONBLOCK`). The shim forces this.
- KWin puts an in-fence on the virtual plane 127. The shim removes plane 127 from the request and waits for that fence in userspace (otherwise `drm_atomic_helper_wait_for_fences` WARN, then a hang).
- `drmoff` must **not** turn off state Android left behind (that hung the SoC). It acts only if every active plane is one of ours.
- `LSW_FORCE_PREFERRED=1` picks 120 Hz (KWin asks for 144 Hz).
- Debug helpers in the shim: `LSW_CLOG` (O_SYNC commit log), `LSW_STATS` (fps), `LSW_DUMP_SEC` / `LSW_DUMP_EVERY` (framebuffer to `/tmp/lsw-fb.ppm`, 8-bit only).

## Session

- Plasma needs a system D-Bus and logind: a bare chroot stalls at the splash screen. Running inside the Droidspaces container gives systemd, logind, polkit, NetworkManager and UPower.
- `startkderc`: `systemdBoot=false` (no systemd user session).
- Xwayland needs `/tmp/.X11-unix` mode 1777, otherwise Xwayland and with it `ksmserver` fail.
- KWin must not use logind (no real seat/session): the shim's constructor with `LSW_KWIN_NO_SYSBUS=1` hides the system bus from `kwin_wayland` only.
- The session runs as the desktop user (`setpriv --reuid=1000`). The extra D-Bus socket `/var/lib/lsw-dbus/system_bus_socket` (drop-in `dbus.socket.d/lsw.conf`) is mode 0666.

## Input

- Create device nodes with major:minor read from `/sys/class/input/eventN/dev` (minor is 64 + N, not N). Wrong minors = no input; this was first misread as a read-only `/sys` problem.
- libinput needs udev properties: `lsw-inputd` writes `/run/udev/data/c13:<minor>` entries.
- Pen (`NVTCapacitivePen`) is ignored by libinput: "missing tablet capabilities: resolution". Not solved.

## Wi-Fi and network

- Android brings `wlan0` down when `system_server` stops. In Linux mode the container's `wpa_supplicant -u` + NetworkManager manage it.
- Android's leftover policy rules route traffic to the (dead) VPN table: add `ip rule add pref 50 lookup main`; `restore.sh` removes it.
- NetworkManager config in `/run/NetworkManager/conf.d/90-lsw.conf` (dns=none, only wlan0, no connectivity check, no MAC randomisation). DNS: `/etc/resolv.conf` → `/run/droidspaces/resolv.conf`.
- The container's NetworkManager must stay masked while Android runs (it would fight Android for wlan0); `p59-inner.sh` unmasks and starts it for the session.
- `mknm.py` imports Android's `WifiConfigStore.xml` (control-character references sanitised, SSIDs over 32 bytes skipped). It prints counts only.

## GPU

- Path: OpenGL → **Zink** → Vulkan **Turnip** → KGSL (Android's GPU driver). The kernel is 6.1 with implicit sync only.
- KWin uses libepoxy, which loads Mesa's `libGLESv2` directly, so KWin works with any build.
- **Qt apps link `libOpenGL.so.0` (glvnd).** With a Mesa built `-Dglvnd=disabled`, glvnd does not know the context, `glGetString` returns NULL and every Qt Quick app segfaults in `QEGLPlatformContext::hasExtension`. That killed ksplash and kded6 and stalled Plasma after kcminit. Fix: build with `-Dglvnd=enabled` (`build-mesa-glvnd.sh`, prefix `/opt/mesa-zink-glvnd`) and set `__EGL_VENDOR_LIBRARY_FILENAMES=/opt/mesa-zink-glvnd/share/glvnd/egl_vendor.d/50_mesa.json`.
- `LSW_GPU` in `p59-inner.sh`: `0` CPU only, `1` old non-glvnd build everywhere (broken), `2` KWin only (shim constructor `LSW_GPU_KWIN` sets the env inside `kwin_wayland_wrapper`), `3` all apps on the glvnd build.
- Also needed: `KWIN_NO_TIMER_QUERY=1`, `XWAYLAND_NO_GLAMOR=1` (glamor on Zink stalls Xwayland), `TU_DEBUG=noconform`.
- Harmless noise: KGSL "CP: AHB bus error" 4× per Vulkan client start; "ZINK: no pdev matches DRM 226:128".

## Brightness

- PowerDevil (installed in the container) uses `org.kde.powerdevil.backlighthelper`, a root helper started by the system bus. Two things make it work: the container's `/sys` is remounted read-write for the session, and polkit rule `51-lsw-user-power.rules` allows the desktop user (no logind seat, so polkit sees an inactive session).
- PowerDevil's scale is 0–10000; the panel is raw 0–4095.
- `POWERDEVIL_NO_DDCUTIL=1` skips DDC/CI probing of I2C buses. `power-profiles-daemon` is masked.
- `powerdevilrc` turns off idle dimming, display power-off and suspend: panel power-off through the 4-strip shim and suspend under the Android kernel are untested.

## Hotplug and background service (`lsw-inputd`)

- There is no `udevd` in the session, so `lsw-inputd` follows kernel uevents, creates nodes and udev database entries, and re-sends the events in libudev's netlink format. `/run/udev/control` must exist before KWin starts, or libudev never subscribes.
- It handles input devices, DRM change events (Qualcomm's DP driver sends no `HOTPLUG=1`), USB block devices and USB sound cards. Only devices on a USB path are touched: never the tablet's own flash or sound card.
- Devices are classified by capability, except built-in parts that must stay away from the desktop.
- It also watches the pen charger's kernel log line, the Bluetooth pen's gestures, the case magnet (`hall_irq` sends key `0xfc` on close and `0xfd` on open, not a lid switch) and sets tablet mode from the keyboard cover, because KWin's own check counts the virtual gesture device as a mouse.
- Android reports charging current as negative, which UPower reads as discharging; a bind mount over `current_now` presents it with the sign flipped.

## External display

- `DP-1` takes one plane and normal commits (plane 92 on CRTC 265), so commits that mention only DP-1 bypass the panel's strip and full-state handling. Enabled with `LSW_DP=1`. Tested at 2560x1440.
- The panel turns off with DPMS: `ACTIVE=0` needs every strip off, and the panel stays off until the compositor asks for `ACTIVE=1`.

## Bluetooth

- Android's Bluetooth HAL keeps running while Linux is up. `bt-hal-bridge` (from bluebinder, GPL-2.0-or-later) talks to it over hwbinder and connects it to BlueZ. This kernel has no VHCI, so the bridge uses a pseudo-terminal with the H4 line discipline.
- It runs outside the container, because the container's `/dev/hwbinder` does not reach Android's service manager.
- Pairings are copied from Android's `bt_config.conf` before each session (`import-bt-bond.py`). Classic link keys are stored in the opposite byte order from BlueZ.
- WirePlumber's seat monitoring is disabled (no logind seat). A2DP works. Headset microphones record silence: SCO audio does not travel over this path.

## Shortcuts

- KernelSU module action: zygote stop kills the KernelSU app's process group, so `action.sh` moves itself to the root cgroup (`echo $$ > /sys/fs/cgroup/cgroup.procs`) and starts `run.sh` with `setsid`.
- Termux:Widget must have the same signing certificate as Termux (GitHub vs F-Droid builds differ; `tools/apkcert.py` compares). Termux needs "Display over other apps".
- "Back to Android" in KDE touches `/var/lib/lsw/back-to-android` (owned by the desktop user); a root watcher started by `run.sh` sees it and runs `restore.sh`.

## Pitfalls

- uid 1000 = Android `system` in the host PID namespace. Never kill by uid; `restore.sh` kills by cgroup and by name.
- A `pkill -f` pattern can match your own `adb shell` command line.
- Wait loops that read the last line of `run.log` must compare run counts, not just "health ok".
- Logs are lost on hard hangs unless synced; pstore sometimes keeps the kernel log.
