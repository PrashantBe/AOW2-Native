#!/bin/bash
# Runs PortMaster's own submission check on port/artofwar2 and builds the release zip
# reviewers and testers use. Fetches only PortMaster-New's tools and release lists.
# Run tools/arm64/build.sh first. Output: dist/artofwar2.zip
set -eu
cd "$(dirname "$0")/.."
project="$PWD"
work=${PM_WORK:-/tmp/portmaster-check}
raw=https://raw.githubusercontent.com/PortsMaster/PortMaster-New/main
mkdir -p "$work/tools/libs" "$work/releases" "$work/runtimes"
cd "$work"
for f in tools/build_release.py tools/libs/util.py SOURCE_SETUP.txt; do curl -fsSL -o "$f" "$raw/$f"; done
[ -d .git ] || git init -q .
if [ ! -f releases/manifest.json ]; then
  (cd releases && for f in ports.json ports_status.json manifest.json; do
     curl -fsSL -o "$f" "https://github.com/PortsMaster/PortMaster-New/releases/latest/download/$f"; done)
fi
rm -rf ports && mkdir ports
cp -r "$project/port/artofwar2" ports/
cp "$project/build-arm64/aow2" ports/artofwar2/artofwar2/aow2.aarch64
python3 tools/build_release.py --do-check
python3 tools/build_release.py --quick-build artofwar2 >/dev/null
mkdir -p "$project/dist" && cp releases/artofwar2.zip "$project/dist/"
echo "PortMaster check passed. Release zip: dist/artofwar2.zip"
