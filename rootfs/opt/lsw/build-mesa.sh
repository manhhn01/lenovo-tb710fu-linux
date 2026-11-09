#!/bin/bash
# runs inside the container as root
set -e
cd /usr/local/src/mesa-26.2.3
python3 /opt/lsw/zink-patch.py
export DEBIAN_FRONTEND=noninteractive
apt-get install -y -qq libx11-xcb-dev libxext-dev libxfixes-dev libxxf86vm-dev libxrandr-dev libxshmfence-dev libxcb-glx0-dev libxcb-dri2-0-dev libxcb-dri3-dev libxcb-present-dev libxcb-sync-dev libxcb-randr0-dev libwayland-dev wayland-protocols libwayland-egl-backend-dev libdrm-dev libexpat1-dev libzstd-dev byacc flex bison >/dev/null
rm -rf build-zink
meson setup build-zink --prefix=/opt/mesa-zink --buildtype=release \
  -Dgallium-drivers=zink -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl,msm \
  -Dplatforms=x11,wayland -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled \
  -Dglx=dri -Dglvnd=disabled -Dllvm=disabled -Dvideo-codecs= -Dgallium-va=disabled \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dbuild-tests=false
nice -n 10 ninja -C build-zink -j3
ninja -C build-zink install
echo BUILD-DONE
