#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN="${OPENXECHAIN:-$ROOT/.openxechain/sysroot}"
CLANG="$TOOLCHAIN/bin/clang"
SYNTHXEX="$TOOLCHAIN/bin/synthxex"

if [[ ! -x "$CLANG" ]]; then
  echo "OpenXeChain clang not found at: $CLANG" >&2
  echo "Set OPENXECHAIN=/path/to/OpenXeChain/sysroot or run scripts/build-toolchain.sh" >&2
  exit 1
fi
if [[ ! -x "$SYNTHXEX" ]]; then
  echo "SynthXEX not found at: $SYNTHXEX" >&2
  exit 1
fi

if [[ ! -f "$ROOT/third_party/stb_image.h" || ! -f "$ROOT/third_party/stb_image_resize.h" || ! -f "$ROOT/third_party/stb_image_write.h" ]]; then
  "$ROOT/scripts/fetch-stb.sh"
fi

rm -rf "$ROOT/build"
mkdir -p "$ROOT/build"

# OpenXeChain installs a clang.cfg next to clang that supplies its Xbox 360
# sysroot, xecorelib, Newlib and compiler-rt defaults.
"$CLANG" \
  -std=c11 -O2 -ffunction-sections -fdata-sections \
  -I"$ROOT/third_party" \
  "$ROOT/src/main.c" \
  -Wl,/opt:ref \
  -o "$ROOT/build/ImageResizer.exe"

"$SYNTHXEX" \
  --input "$ROOT/build/ImageResizer.exe" \
  --output "$ROOT/build/default.xex" \
  --type title

cp "$ROOT/config.ini" "$ROOT/build/config.ini"

echo "Built: $ROOT/build/default.xex"
