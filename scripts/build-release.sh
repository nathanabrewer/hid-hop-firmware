#!/usr/bin/env bash
#
# build-release.sh — clean, reproducible firmware build + DFU package for distribution.
#
# Builds the firmware in the Docker toolchain (pristine), then produces a Nordic
# Secure DFU package and extracts the raw artifacts the web flasher consumes.
#
# Usage:
#   scripts/build-release.sh [-b BOARD] [-o OUTDIR]
#
# Defaults: BOARD=raytac_mdbt50q_cx_40, OUTDIR=dist
#
# Requires: docker (with brewer-hid-bridge-builder image) and host `nrfutil`
#           with the nrf5sdk-tools command installed (`nrfutil install nrf5sdk-tools`).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
cd "$ROOT"

BOARD="raytac_mdbt50q_cx_40"
OUTDIR="dist"
IMAGE="brewer-hid-bridge-builder:latest"
APP_VERSION="${APP_VERSION:-1}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -b|--board)  BOARD="$2"; shift 2;;
    -o|--out)    OUTDIR="$2"; shift 2;;
    -h|--help)   sed -n '2,18p' "$0"; exit 0;;
    *) echo "Unknown option: $1" >&2; exit 1;;
  esac
done

echo ">> Building $BOARD (pristine) in Docker…"
rm -rf build
CMAKE_ARGS=""
if [[ -d "boards/arm/$BOARD" ]]; then
  CMAKE_ARGS="-- -DBOARD_ROOT=/workspace"
fi
docker run --rm -v "$ROOT:/workspace" -w /workspace "$IMAGE" \
  west build -b "$BOARD" --pristine $CMAKE_ARGS

if [[ ! -f build/zephyr/zephyr.hex ]]; then
  echo "!! Build failed: build/zephyr/zephyr.hex missing" >&2
  exit 1
fi
echo ">> Build OK."

mkdir -p "$OUTDIR"
PKG="$OUTDIR/hid-hop-${BOARD}.zip"

echo ">> Packaging Nordic Secure DFU bundle…"
nrfutil pkg generate \
  --hw-version 52 --sd-req 0x00 \
  --application build/zephyr/zephyr.hex \
  --application-version "$APP_VERSION" \
  "$PKG"

echo ">> Extracting flasher artifacts to docs/firmware/ …"
FW_DIR="docs/firmware"
mkdir -p "$FW_DIR"
# unzip the DFU package: zephyr.bin (image), zephyr.dat (init packet), manifest.json
unzip -o "$PKG" -d "$FW_DIR" >/dev/null
# record provenance for the web page
GIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
SIZE="$(wc -c < build/zephyr/zephyr.bin | tr -d ' ')"
cat > "$FW_DIR/build-info.json" <<EOF
{
  "board": "$BOARD",
  "git_sha": "$GIT_SHA",
  "app_version": $APP_VERSION,
  "bin_bytes": $SIZE
}
EOF

echo ""
echo "================ done ================"
echo "DFU package : $PKG"
echo "Flasher dir : $FW_DIR/ (zephyr.bin, zephyr.dat, manifest.json, build-info.json)"
echo "Board       : $BOARD   commit $GIT_SHA   image $((SIZE/1024)) KB"
