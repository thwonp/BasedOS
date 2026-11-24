# POSIX syntax check of every BasedOS shell script, with dash (stricter than BusyBox ash).
cd "$(dirname "$0")/../.."
rc=0
for f in initrd/basedos/init overlay/etc/init.d/* overlay/usr/sbin/mount-frontend overlay/usr/sbin/frontend-session overlay/usr/sbin/basedos-suspend overlay/usr/sbin/basedos-udev overlay/usr/sbin/basedos-pipewire overlay/usr/sbin/basedos-bluetooth overlay/usr/sbin/basedos-boot overlay/usr/sbin/basedos-menu overlay/usr/sbin/basedos-update overlay/usr/lib/basedos/functions overlay/usr/share/udhcpc/default.script tools/install-dev.sh tools/basedos-setup.sh tools/recovery/init tools/dev/update-basedos.sh; do
	dash -n "$f" || { echo "SYNTAX: $f"; rc=1; }
done
# What runs on stock from its Ports (bash there).
for f in overlay/usr/share/basedos/stock/*; do
	bash -n "$f" || { echo "SYNTAX: $f"; rc=1; }
done
# screen.sh is also sourced by basedos-setup.sh, under stock's /bin/sh (BusyBox ash).
dash -n overlay/usr/share/basedos/stock/screen.sh || { echo "SYNTAX (sh): screen.sh"; rc=1; }
exit $rc
