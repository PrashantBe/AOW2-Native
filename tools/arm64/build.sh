#!/bin/bash
# Builds build-arm64/aow2 for the handheld. Run from WSL or Linux with Docker.
set -eu
cd "$(dirname "$0")/../.."
docker build -q -t aow2-arm64 tools/arm64 >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" -w /src aow2-arm64 bash -c '
  cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=/opt/toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
        -DAOW2_PROTOTYPE=OFF -DAOW2_STATIC_DEPS=ON >/dev/null &&
  cmake --build build-arm64 -j &&
  aarch64-linux-gnu-strip build-arm64/aow2 &&
  file build-arm64/aow2 &&
  aarch64-linux-gnu-readelf -d build-arm64/aow2 | grep NEEDED'
