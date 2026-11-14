# Cross-compiler for the RG40XXV and other ARM64 Linux handhelds.
# The binary then needs glibc 2.36 or newer on the device. Check the device's version if it fails to start.
FROM debian:bookworm
RUN dpkg --add-architecture arm64 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      crossbuild-essential-arm64 cmake make pkg-config file \
      libsdl2-dev:arm64 libpng-dev:arm64 zlib1g-dev:arm64 \
 && rm -rf /var/lib/apt/lists/*
COPY toolchain.cmake /opt/toolchain.cmake
