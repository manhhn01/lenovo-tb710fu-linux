# Needs the Mesa 26.2.3 source tree at /usr/local/src/mesa-26.2.3 (kept off the tablet to save space).
set -e
cd /usr/local/src/mesa-26.2.3
dpkg -s libglvnd-dev >/dev/null 2>&1 || apt-get install -y libglvnd-dev >/tmp/apt.log 2>&1
rm -rf build-glvnd
meson setup build-glvnd -Dgallium-drivers=zink -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl,msm -Dplatforms=x11,wayland -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled -Dglx=dri -Dglvnd=enabled -Dllvm=disabled -Dvideo-codecs= -Dgallium-va=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dbuild-tests=false -Dprefix=/opt/mesa-zink-glvnd -Dbuildtype=release > /tmp/bld.log 2>&1
ninja -C build-glvnd -j6 >> /tmp/bld.log 2>&1
ninja -C build-glvnd install >> /tmp/bld.log 2>&1
echo BUILD-OK
