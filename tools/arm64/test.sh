#!/bin/bash
# Boots the ARM64 build to its main menu under QEMU emulation. Run build.sh first.
set -eu
cd "$(dirname "$0")/../.."
docker build -q -t aow2-arm64-test tools/arm64/test-image >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" -w /src aow2-arm64-test bash -c '
  echo "size $(stat -c %s build-arm64/aow2) bytes"
  rm -rf build-arm64/test-saves
  qemu-aarch64 -L / build-arm64/aow2 ArtOfWar2__Nokia_5800.jar --headless 20 --saves build-arm64/test-saves \
      --tap 8:20,340 --shot 19:build-arm64/arm64-menu.png --min-frames 80'
echo "ARM64 boot test passed. Screenshot: build-arm64/arm64-menu.png"
