# BasedOS's build host: what tools/buildroot, tools/sdl2, tools/bash and tools/wsl/build.sh need,
# on the Ubuntu 24.04 they are written for. A newer host compiler (GCC 15+) can break
# Buildroot 2025.02's host packages; this keeps every Linux host on the same one.
FROM docker.io/library/ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
	build-essential file wget curl ca-certificates cpio unzip rsync bc python3 perl git patch \
	xz-utils bzip2 gzip libncurses-dev dash squashfs-tools \
	cmake ninja-build pkg-config gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
	libdrm-dev libgbm-dev libasound2-dev \
	&& rm -rf /var/lib/apt/lists/*
