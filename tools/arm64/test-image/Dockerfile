# Runs ARM64 builds on a PC through QEMU, inside an Ubuntu 20.04 ARM64 userland
# (glibc 2.31, SDL 2.0.10), older than any current handheld firmware.
# QEMU comes from Debian 13 because older QEMU (7.2) hangs on thread creation.
FROM debian:trixie AS qemu
RUN apt-get update && apt-get install -y --no-install-recommends qemu-user-static

FROM ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive
RUN sed -i 's/^deb http/deb [arch=amd64] http/' /etc/apt/sources.list \
 && for suite in focal focal-updates focal-security; do \
      echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports $suite main universe" >> /etc/apt/sources.list; \
    done \
 && dpkg --add-architecture arm64 \
 && apt-get update \
 && apt-get install -y --no-install-recommends libc6:arm64 libstdc++6:arm64 libsdl2-2.0-0:arm64 binutils \
 && rm -rf /var/lib/apt/lists/*
COPY --from=qemu /usr/bin/qemu-aarch64-static /usr/bin/qemu-aarch64
