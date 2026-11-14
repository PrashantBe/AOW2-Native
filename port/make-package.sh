#!/bin/bash
# Assembles dist/ the way PortMaster installs the port: copy its contents into roms/ports/.
# The game JAR is not included. Players copy their own into artofwar2/gamedata/.
set -eu
cd "$(dirname "$0")/.."
[ -f build-arm64/aow2 ] || { echo "Run tools/arm64/build.sh first"; exit 1; }
rm -rf dist && mkdir -p dist
cp "port/artofwar2/Art of War 2 (Native).sh" dist/
cp -r port/artofwar2/artofwar2 dist/
cp port/artofwar2/port.json port/artofwar2/README.md port/artofwar2/gameinfo.xml port/artofwar2/screenshot.png port/artofwar2/cover.png dist/artofwar2/
cp build-arm64/aow2 dist/artofwar2/aow2.aarch64
(cd dist && find . -type f | sort)
