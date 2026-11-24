#!/bin/sh
# Run a BasedOS build script in the build container (tools/container/Dockerfile), on any Linux
# host with podman or docker:
#   tools/container/run.sh tools/buildroot/build.sh build
#   tools/container/run.sh tools/wsl/build.sh
# The repo is /repo inside; the work folders the scripts keep in $HOME (Buildroot's tree and
# downloads, basedos-work, ...) go to $BASEDOS_HOST_WORK (default ~/basedos-host), which needs
# some 30 GB. Files come out owned by you.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
HW=${BASEDOS_HOST_WORK:-$HOME/basedos-host}
IMAGE=basedos-build
ENGINE=$(command -v podman || command -v docker) || { echo "needs podman or docker" >&2; exit 1; }
mkdir -p "$HW"
"$ENGINE" image inspect "$IMAGE" > /dev/null 2>&1 ||
	"$ENGINE" build -t "$IMAGE" -f "$REPO/tools/container/Dockerfile" "$REPO/tools/container"
# Rootless podman: keep-id maps you to yourself inside. Docker: run as your uid.
case "$("$ENGINE" --version)" in
*podman*) USER_OPT="--userns=keep-id" ;;
*) USER_OPT="--user $(id -u):$(id -g)" ;;
esac
# label=disable: no SELinux relabelling of the repo and the work folder.
exec "$ENGINE" run --rm -i $USER_OPT --security-opt label=disable \
	-v "$REPO:/repo" -v "$HW:/work" -e HOME=/work -w /repo "$IMAGE" bash "$@"
