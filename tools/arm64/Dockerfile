# Cross-compiler for the RG40XXV and other ARM64 Linux handhelds.
# Ubuntu 20.04 (glibc 2.31, SDL 2.0.10) is the oldest base PortMaster recommends, so the
# binary also starts on older firmware such as ArkOS. Newer firmware runs it unchanged.
FROM ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive
# amd64 packages come from the main archive, arm64 ones from ports.ubuntu.com.
RUN sed -i 's/^deb http/deb [arch=amd64] http/' /etc/apt/sources.list \
 && for suite in focal focal-updates focal-security; do \
      echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports $suite main universe" >> /etc/apt/sources.list; \
    done \
 && dpkg --add-architecture arm64 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      g++-aarch64-linux-gnu binutils-aarch64-linux-gnu cmake make pkg-config file ca-certificates \
      libsdl2-dev:arm64 libpng-dev:arm64 zlib1g-dev:arm64 \
 && rm -rf /var/lib/apt/lists/*
COPY toolchain.cmake /opt/toolchain.cmake
