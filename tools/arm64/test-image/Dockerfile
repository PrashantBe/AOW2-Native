# Runs ARM64 builds on a PC through QEMU's user-mode emulator.
# Debian 13's QEMU is used because Debian 12's (7.2) hangs on thread creation.
FROM debian:trixie
RUN dpkg --add-architecture arm64 \
 && apt-get update \
 && apt-get install -y --no-install-recommends qemu-user libc6:arm64 libstdc++6:arm64 libsdl2-2.0-0:arm64 \
 && rm -rf /var/lib/apt/lists/*
