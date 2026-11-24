#!/bin/sh
# Full BasedOS build (the system layer, the initrd, the boot scripts, the recovery card's files), in
# WSL or on any Linux host (tools/container/run.sh runs it in the build container).
#   BASEDOS_WORK   work folder (default ~/basedos-work)
#   BASEDOS_STOCK  the prepared stock build (default prepared/rocknix-20260613-c8ed06c)
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
W=${BASEDOS_WORK:-$HOME/basedos-work}
STOCK=${BASEDOS_STOCK:-$REPO/prepared/rocknix-20260613-c8ed06c}
mkdir -p "$W" "$REPO/out"
# A build that stops part-way must not leave a release-able out/: the manifest of a whole build
# (out/build.sha256, written last) goes first; tools/wsl/release.sh needs it.
rm -f "$REPO/out/build.sha256"
# The stock build's name, without the carriage return a Windows-written BUILD file carries.
tr -d '\r' < "$STOCK/BUILD" > "$W/STOCK_BUILD"
# Stock's root, unpacked once per stock build (the links into /stock are made against it, and the
# BUILD_ID the layer needs comes from it; lib32-gaps reads it). stock-root.build says which build
# it is, and is written only once an unpack has finished: another STOCK, or an unpack that didn't
# finish, unpacks again.
if [ "$(cat "$W/stock-root.build" 2>/dev/null)" != "$(cat "$W/STOCK_BUILD")" ] || [ ! -d "$W/stock-root/usr/lib" ]; then
	rm -rf "$W/stock-root" "$W/stock-root.build"
	sh "$REPO/tools/wsl/unpack-stock.sh" "$STOCK/SYSTEM" "$W/stock-root"
	cp "$W/STOCK_BUILD" "$W/stock-root.build"
fi

# sdl2_current <prepared dir> <arch>: 0 if that SDL2 build is of the SDL and librga commits and
# the patches tools/sdl2 has now (its build-info records them), so it needn't be built again.
sdl2_current() {
	_i=$1/libSDL2.build-info
	[ -f "$_i" ] || return 1
	_a=$(sed -n 's/^arch //p' "$_i")
	[ "${_a:-aarch64}" = "$2" ] || return 1
	for _c in SDL_COMMIT:SDL RGA_COMMIT:librga; do
		_v=$(sed -n "s/^${_c%%:*}=\([0-9a-f]*\).*/\1/p" "$REPO/tools/sdl2/build.sh")
		grep -q "^${_c#*:} $_v " "$_i" || return 1
	done
	_n=0
	for _p in "$REPO"/tools/sdl2/patches/sdl2-patch*; do
		grep -qx "patch ${_p##*/} $(sha256sum < "$_p" | cut -c1-16)" "$_i" || return 1
		_n=$((_n + 1))
	done
	[ "$(grep -c '^patch ' "$_i")" -eq "$_n" ]
}

sdl2_current "$REPO/prepared/sdl2" aarch64 || {
	echo "--- SDL2 (64-bit): building (missing or out of date)"
	rm -rf "$REPO/prepared/sdl2"
	bash "$REPO/tools/sdl2/build.sh" "$STOCK/SYSTEM" "$REPO/prepared/sdl2" 270
}
# BasedOS's userland (glibc, OpenSSH, wpa_supplicant and wpa_cli, ALSA, ...): about an hour the first
# time, then only what changed.
bash "$REPO/tools/buildroot/build.sh" build
# Its 32-bit libraries for PortMaster's 32-bit ports (docs/lib32.md), and SDL2 for them, built with
# that root's toolchain.
bash "$REPO/tools/buildroot/build.sh" --armhf build
sdl2_current "$REPO/prepared/sdl2-armhf" armhf || {
	echo "--- SDL2 (32-bit): building (missing or out of date)"
	rm -rf "$REPO/prepared/sdl2-armhf"
	SDL2_ARCH=armhf bash "$REPO/tools/sdl2/build.sh" "$STOCK/SYSTEM" "$REPO/prepared/sdl2-armhf" 270
}
# bash: built again when tools/bash/build.sh names another version than the one in prepared/.
BASH_VER=$(sed -n 's/^VER=//p' "$REPO/tools/bash/build.sh")
grep -q "^bash $BASH_VER," "$REPO/prepared/bash/build-info" 2>/dev/null || {
	echo "--- bash $BASH_VER: building (missing or another version)"
	rm -rf "$REPO/prepared/bash"
	bash "$REPO/tools/bash/build.sh" "$REPO/prepared/bash"
}
sh "$REPO/tools/wsl/build-rootfs.sh" "$REPO" "$W" "$REPO/prepared/alpine/busybox.static" "$REPO/prepared/alpine/busybox.list"
STOCK_BUILD_ID=$(cat "$W/system/usr/share/basedos/needs-stock")
mkdir -p "$REPO/out"
cp "$W/rootfs.squashfs" "$REPO/out/rootfs.squashfs"
# (Older builds' vendor sets and boot.env: BasedOS no longer has either.)
rm -rf "$REPO"/out/vendor-* "$REPO/out/boot.env"
# BasedOS's boot files: the initrd and the boot script (setup installs them; updates don't carry
# them). The system layer must not need a newer initrd interface than this one gives.
rm -f "$W/initrd.uimg.new"
python3 "$REPO/tools/mkboot.py" initrd "$REPO/prepared/alpine/busybox.static" "$W/initrd.uimg.new" >/dev/null
mv "$W/initrd.uimg.new" "$REPO/out/initrd.uimg"
INITRD_ABI=$(sed -n 's/^ABI=\([0-9][0-9]*\)$/\1/p' "$REPO/initrd/basedos/init")
NEEDS_INITRD=$(cat "$W/system/usr/share/basedos/needs-initrd")
case "$INITRD_ABI:$NEEDS_INITRD" in *[!0-9:]*|:*|*:) echo "initrd ABI '$INITRD_ABI' or needs-initrd '$NEEDS_INITRD' isn't a number" >&2; exit 1 ;; esac
[ "$NEEDS_INITRD" -le "$INITRD_ABI" ] || { echo "the system layer needs initrd interface $NEEDS_INITRD; initrd/basedos/init gives $INITRD_ABI" >&2; exit 1; }
# BasedOS's own boot script (initrd/boot.cmd; setup puts it in place of stock's).
rm -f "$W/boot.scr.new"
python3 "$REPO/tools/mkboot.py" script "$REPO/initrd/boot.cmd" "$W/boot.scr.new"
mv "$W/boot.scr.new" "$REPO/out/boot.scr"
# The recovery card's initrd and boot script (tools/recovery), and the setup script itself: with
# rootfs.squashfs, initrd.uimg and boot.scr, that's the release folder basedos-setup runs from.
# release.info: what basedos-setup checks before it asks anything or downloads the rest: this
# build, and the stock build it runs on (its BUILD_ID, and the SHA-256 of the stock files BasedOS
# boots and mounts: the kernel, the DTBs, SYSTEM), so the device's own stock is matched first.
{
	sed -n 's/^BASEOS_\(VERSION\|BUILD\)=/\1=/p' "$W/system/etc/baseos-release"
	echo "STOCK_BUILD=$(cat "$W/STOCK_BUILD")"
	echo "STOCK_BUILD_ID=$STOCK_BUILD_ID"
	for f in Image rk3576-gkd-atom.dtb rk3576-gkd-atom-hdmi.dtb rk3576-gkd-atom-usbdp.dtb SYSTEM; do
		# Checked here: a missing file would give an empty hash inside $(...) without stopping
		# the build, and a release no device can install.
		h=$(sha256sum < "$STOCK/$f" | cut -c1-64)
		case "$h" in *[!0-9a-f]*|"") h="" ;; esac
		[ ${#h} -eq 64 ] || { echo "release.info: no SHA-256 for stock's $f ($STOCK/$f)" >&2; exit 1; }
		echo "P1 $h $f"
	done
} > "$REPO/out/release.info"
[ "$(grep -c '^P1 [0-9a-f]\{64\} ' "$REPO/out/release.info")" -eq 5 ] || { echo "release.info: stock's file hashes missing" >&2; exit 1; }
# The RK3576 maskrom USB loader (Rockchip's rkbin DDR init and usbplug, packed by
# tools/maskrom/mk-usb-loader.py) and rkbin's licence: setup puts them on every recovery card, so a
# maskrom restore from a PC needs nothing else (docs/recovery.md). Distributable under rkbin's
# licence, which travels with it. Kept in prepared/maskrom/, checked against its known SHA-256.
USB_LOADER=rk3576_usb_loader_ddr1.13_usbplug1.04.bin
[ "$(sha256sum < "$REPO/prepared/maskrom/$USB_LOADER" | cut -c1-64)" = f2bac2f2e82ea0997d096aa7062629a3d1cc2af918d7c304db3610a93a82be97 ] ||
	{ echo "prepared/maskrom/$USB_LOADER missing or not the known build" >&2; exit 1; }
cp "$REPO/prepared/maskrom/$USB_LOADER" "$REPO/prepared/maskrom/LICENSE.rkbin.txt" "$REPO/out/"
sh "$REPO/tools/recovery/build.sh" "$REPO" "$W" "$REPO/prepared/alpine/busybox.static" "$REPO/out"
cp "$REPO/tools/basedos-setup.sh" "$REPO/out/basedos-setup.sh"
rm -f "$REPO"/out/baseos-*.bosupd
# The update payload: the system layer, nothing of stock's (publishable).
python3 "$REPO/tools/mkupdate.py" "$W/rootfs.squashfs" "$NEEDS_INITRD" 	"$STOCK_BUILD_ID" "$(cat "$REPO/VERSION")" 	"$(sed -n 's/^BASEOS_BUILD=//p' "$W/system/etc/baseos-release")" "$REPO/out"
for r in harvest-report.txt base32-report.txt lib32-gaps.txt; do
	cp "$W/$r" "$REPO/out/$r" 2>/dev/null || true
done
# The manifest of this whole build, last: its name and every release file's SHA-256. release.sh
# publishes only files that match it, so files from builds that didn't finish never mix in.
BUILD_NAME=$(sed -n 's/^BASEOS_BUILD=//p' "$W/system/etc/baseos-release")
(cd "$REPO/out" && {
	echo "# build $BUILD_NAME"
	sha256sum release.info rootfs.squashfs initrd.uimg boot.scr recovery-initrd.uimg recovery.scr basedos-setup.sh \
		"$USB_LOADER" LICENSE.rkbin.txt baseos-gkd350hultra-*.bosupd
} > build.sha256.new && mv build.sha256.new build.sha256)
