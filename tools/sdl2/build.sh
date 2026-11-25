#!/bin/bash
# BasedOS's SDL2: the build dArkMoss makes for its rotated-panel units (build_sdl2.sh), cross-built
# in WSL. SDL 2.32.10 with ArkOS's SDL2 patches (christianhaitian/rk3566_core_builds, MIT) and the
# Miniloong RGA rotation patch: every DSI window is turned by the RGA before scanout, HDMI and
# Vulkan windows are left alone. The 350H Ultra's 1440x1600 panel is shown as 1600x1440.
# BasedOS's patch 0005 turns it on the GPU instead when there is no /dev/rga (mainline kernels);
# SDL_KMSDRM_ROTATION=gpu|rga picks one, SDL_KMSDRM_ROTATION_STATS=1|2|3 times it.
# Patch 0006 finds keyboards, mice and touch devices without udev (BasedOS has none): one scan of
# /dev/input at start, classified as udev's input_id does.
# Patch 0007 keeps a window's GBM/EGL surfaces when SDL_SetWindowSize or SDL_SetWindowFullscreen
# leaves the display mode as it is: KMSDRM always makes them at the mode's size, so recreating
# them only discarded the frame. An SDL 1.2 game that sets its video mode every frame (A7Xpg)
# showed nothing but black.
#
# librga is built here too, from Rockchip's open source (JeffyCN/mirrors linux-rga-multi, 1.10.5,
# Apache-2.0): the stock image's librga is 1.3.2, older than the RK3576, and the kernel's RGA
# driver (multicore v1.3.9) rejects its requests ("invalid setting core by user").
#
# Needs (Ubuntu 24.04 in WSL):
#   sudo apt-get install cmake ninja-build pkg-config gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
#        libdrm-dev libgbm-dev libasound2-dev squashfs-tools
# Only the -dev packages' headers are used (they don't depend on the architecture). libdrm, libgbm
# and libasound to link against come from the stock SYSTEM image, as the rootfs harvest does;
# nothing vendor-made ends up in the repo or the output.
#
#   build.sh <stock SYSTEM squashfs> <out dir> [270|90]
#   SDL2_ARCH=armhf build.sh ...  the same for 32-bit ports (/usr/lib32), with BasedOS's 32-bit
#                                 Buildroot toolchain (tools/buildroot/build.sh --armhf build)
#                                 and stock's 32-bit libraries to link against; it also
#                                 loads libpulse (PulseAudio's client, from that Buildroot root):
#                                 32-bit programs can't load PipeWire's 64-bit ALSA plugin, so
#                                 they reach PipeWire's PulseAudio server instead
# tools/wsl/build.sh runs it into prepared/sdl2 when that is missing.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SYSTEM=$1
OUT=$2
ROT=${3:-270}
ARCH=${SDL2_ARCH:-aarch64}
case "$ARCH" in
aarch64)
    CROSS=aarch64-linux-gnu PROC=aarch64 STOCK_LIB=usr/lib PULSE=OFF
    W=${SDL2_WORK:-$HOME/basedos-sdl2} ;;
armhf)
    BR32=${BR32_OUT:-$HOME/basedos-br/out-armhf}
    CROSS=${SDL2_CROSS:-$BR32/host/bin/arm-buildroot-linux-gnueabihf}
    PROC=arm STOCK_LIB=usr/lib32 PULSE=ON
    W=${SDL2_WORK:-$HOME/basedos-sdl2-armhf} ;;
*) echo "SDL2_ARCH must be aarch64 or armhf" >&2; exit 1 ;;
esac

SDL_COMMIT=5d249570393f7a37e037abf22cd6012a4cc56a71   # SDL 2.32.10, ArkOS scripts/sdl2.sh
SDL_EXT=3200.10
RGA_COMMIT=57a1067a246c71fa6c9a355d1668884fda155dd5   # librga 1.10.5, linux-rga-multi

case "$ROT" in 270|90) ;; *) echo "rotation must be 270 or 90" >&2; exit 1 ;; esac
for t in cmake ninja pkg-config $CROSS-gcc $CROSS-g++ unsquashfs git patch; do
    command -v "$t" >/dev/null || { echo "missing $t (see the header)" >&2; exit 1; }
done
for h in /usr/include/libdrm/drm.h /usr/include/xf86drm.h /usr/include/gbm.h /usr/include/alsa/asoundlib.h; do
    [ -f "$h" ] || { echo "missing $h (see the header)" >&2; exit 1; }
done

rm -rf "$W"
SR=$W/sysroot
mkdir -p "$SR/usr/include/rga" "$SR/usr/lib/pkgconfig" "$OUT"

# Headers.
cp -r /usr/include/libdrm /usr/include/alsa "$SR/usr/include/"
cp /usr/include/xf86drm.h /usr/include/xf86drmMode.h /usr/include/gbm.h "$SR/usr/include/"
cat > "$W/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR $PROC)
set(CMAKE_C_COMPILER $CROSS-gcc)
set(CMAKE_CXX_COMPILER $CROSS-g++)
set(CMAKE_FIND_ROOT_PATH $SR /usr/$CROSS)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

# librga from source (it carries its own copy of the libdrm headers).
git init -q "$W/librga"
git -C "$W/librga" fetch -q --depth 1 https://github.com/JeffyCN/mirrors.git "$RGA_COMMIT"
git -C "$W/librga" -c advice.detachedHead=false checkout -q FETCH_HEAD
cmake -S "$W/librga" -B "$W/librga/build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$W/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_BUILD_TARGET=buildroot -DRGA_SOURCE_CODE_TYPE=cpp -DBUILD_WITH_VERSION=true \
    -DRGA_SAMPLES_ENABLE=false -DCMAKE_INSTALL_PREFIX="$W/librga/install"
cmake --build "$W/librga/build"
cmake --install "$W/librga/build" >/dev/null
cp "$W/librga/install/include/"*.h "$SR/usr/include/rga/"
cp -a "$W/librga/install/lib/"librga.so* "$SR/usr/lib/"

# The rest to link against, from stock.
unsquashfs -q -d "$W/stock" "$SYSTEM" \
    "$STOCK_LIB/libdrm.so*" "$STOCK_LIB/libgbm.so*" "$STOCK_LIB/libasound.so*" >/dev/null
cp -a "$W/stock/$STOCK_LIB/"* "$SR/usr/lib/"
for pc in "libdrm:drm:-I\${includedir}/libdrm" "gbm:gbm:" "alsa:asound:"; do
    IFS=: read -r name lib cflags <<EOF
$pc
EOF
    cat > "$SR/usr/lib/pkgconfig/$name.pc" <<EOF
prefix=$SR/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib
Name: $name
Description: $name (stock library, host headers)
Version: 99
Cflags: -I\${includedir} $cflags
Libs: -L\${libdir} -l$lib
EOF
done
# libpulse's headers and library from the 32-bit Buildroot root (only its name is linked against:
# SDL loads it at run time).
if [ "$PULSE" = ON ]; then
    cp -r "$BR32/staging/usr/include/pulse" "$SR/usr/include/"
    cp -a "$BR32/staging/usr/lib/"libpulse.so* "$SR/usr/lib/"
    printf 'prefix=%s/usr\nName: libpulse\nDescription: libpulse (Buildroot armhf)\nVersion: 17.0\nCflags: -I${prefix}/include\nLibs: -L${prefix}/lib -lpulse\n' \
        "$SR" > "$SR/usr/lib/pkgconfig/libpulse.pc"
fi
# SDL's KMSDRM check wants an egl module too; SDL brings its own EGL headers and loads libEGL at
# run time, so it needs no flags.
printf 'Name: egl\nDescription: libEGL (loaded at run time)\nVersion: 99\nCflags:\nLibs:\n' \
    > "$SR/usr/lib/pkgconfig/egl.pc"

# SDL at ArkOS's commit, with ArkOS's two source changes and the patches in glob order.
git clone -q --filter=blob:none https://github.com/libsdl-org/SDL.git "$W/SDL"
cd "$W/SDL"
git -c advice.detachedHead=false checkout -q "$SDL_COMMIT"
for p in "$HERE/patches/"sdl2-patch*; do
    if [ "$ROT" = 90 ] && [ "${p##*/}" = sdl2-patch-0004-miniloong-rotate-kmsdrm.patch ]; then
        sed 's/HAL_TRANSFORM_ROT_270/HAL_TRANSFORM_ROT_90/' "$p" | patch -Np1 -s
    else
        patch -Np1 -s < "$p"
    fi
done
git checkout -q 528b71284f491bcb6ecfd4ab7e00d37b296bd621 -- src/joystick/SDL_gamecontroller.c  # revert CRC joystick changes
git revert -n e5024fae3decb724e397d3c9dbcb744d8c79aac1


# ArkOS's options, except libsamplerate (BasedOS doesn't carry it).
export PKG_CONFIG_LIBDIR="$SR/usr/lib/pkgconfig" PKG_CONFIG_PATH=
cmake -S . -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$W/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-I$SR/usr/include -I$SR/usr/include/libdrm" \
    -DCMAKE_SHARED_LINKER_FLAGS="-L$SR/usr/lib -Wl,-rpath-link,$SR/usr/lib" \
    -DSDL_STATIC=OFF -DSDL_LIBC=ON -DSDL_GCC_ATOMICS=ON -DSDL_ALTIVEC=OFF -DSDL_OSS=OFF \
    -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON -DSDL_JACK=OFF -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_NAS=OFF \
    -DSDL_LIBSAMPLERATE=OFF -DSDL_SNDIO=OFF -DSDL_DISKAUDIO=OFF -DSDL_DUMMYAUDIO=OFF \
    -DSDL_WAYLAND=OFF -DSDL_COCOA=OFF -DSDL_DIRECTFB=OFF -DSDL_VIVANTE=OFF -DSDL_FUSIONSOUND=OFF \
    -DSDL_DUMMYVIDEO=OFF -DSDL_PTHREADS=ON -DSDL_PTHREADS_SEM=ON -DSDL_DIRECTX=OFF \
    -DSDL_CLOCK_GETTIME=OFF -DSDL_RPATH=OFF -DSDL_RENDER_D3D=OFF -DSDL_X11=OFF \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=ON -DSDL_VULKAN=ON -DSDL_KMSDRM=ON -DSDL_PULSEAUDIO=$PULSE -DSDL_PULSEAUDIO_SHARED=$PULSE \
    -DSDL_PIPEWIRE=OFF -DSDL_TEST=OFF
cmake --build build
if [ "$PULSE" = ON ]; then
    grep -q '^#define SDL_AUDIO_DRIVER_PULSEAUDIO_DYNAMIC "libpulse.so.0"' build/include-config-release/SDL2/SDL_config.h ||
        { echo "PulseAudio was not enabled" >&2; exit 1; }
fi
# The rotation lives in the KMSDRM driver, and librga must be linked for it.
grep -q '^#define SDL_VIDEO_DRIVER_KMSDRM 1' build/include-config-release/SDL2/SDL_config.h ||
    { echo "KMSDRM was not enabled" >&2; exit 1; }
$CROSS-readelf -d "build/libSDL2-2.0.so.0.$SDL_EXT" | grep -q 'NEEDED.*librga' ||
    { echo "librga is not linked" >&2; exit 1; }
$CROSS-strip "build/libSDL2-2.0.so.0.$SDL_EXT"

cp "build/libSDL2-2.0.so.0.$SDL_EXT" "$OUT/"
RGA_SO=$(cd "$W/librga/install/lib" && ls librga.so.*.*.*)
cp "$W/librga/install/lib/$RGA_SO" "$OUT/"
$CROSS-strip "$OUT/$RGA_SO"
{
    echo "SDL $SDL_COMMIT (2.32.10)"
    echo "librga $RGA_COMMIT ($RGA_SO)"
    echo "rotation $ROT"
    echo "arch $ARCH"
    echo "pulseaudio $PULSE"
    for p in "$HERE/patches/"sdl2-patch*; do echo "patch ${p##*/} $(sha256sum "$p" | cut -c1-16)"; done
    (cd "$OUT" && sha256sum "libSDL2-2.0.so.0.$SDL_EXT" "$RGA_SO")
} > "$OUT/libSDL2.build-info"
cat "$OUT/libSDL2.build-info"
