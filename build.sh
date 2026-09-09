#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN="${OPENXECHAIN:-$ROOT/.openxechain/sysroot}"

CLANG="$TOOLCHAIN/bin/clang"
SYNTHXEX="$TOOLCHAIN/bin/synthxex"
LIBM="$TOOLCHAIN/ppc-xbox360/lib/libm.a"

if [[ ! -x "$CLANG" ]]; then
    echo "OpenXeChain clang not found: $CLANG" >&2
    exit 1
fi

if [[ ! -x "$SYNTHXEX" ]]; then
    echo "SynthXEX not found: $SYNTHXEX" >&2
    exit 1
fi

if [[ ! -f "$LIBM" ]]; then
    echo "OpenXeChain libm not found: $LIBM" >&2
    echo "Available libm files:" >&2
    find "$TOOLCHAIN" -name 'libm.a' -type f -print >&2 || true
    exit 1
fi

if [[ ! -f "$ROOT/third_party/stb_image.h" || \
      ! -f "$ROOT/third_party/stb_image_resize.h" || \
      ! -f "$ROOT/third_party/stb_image_write.h" ]]; then
    "$ROOT/scripts/fetch-stb.sh"
fi

rm -rf "$ROOT/build"
mkdir -p "$ROOT/build"

echo "Using OpenXeChain:"
"$CLANG" --version

echo
echo "Using math library:"
ls -lh "$LIBM"

echo
echo "Compiling and linking Image Resizer..."

"$CLANG" \
    -std=c11 \
    -O2 \
    -I"$ROOT/third_party" \
    "$ROOT/src/main.c" \
    "$ROOT/src/stb_impl.c" \
    "$LIBM" \
    -o "$ROOT/build/ImageResizer.exe"

if [[ ! -f "$ROOT/build/ImageResizer.exe" ]]; then
    echo "ImageResizer.exe was not created." >&2
    exit 1
fi

echo
echo "PE created:"
ls -lh "$ROOT/build/ImageResizer.exe"

echo
echo "Creating XEX..."

"$SYNTHXEX" \
    --input "$ROOT/build/ImageResizer.exe" \
    --output "$ROOT/build/default.xex" \
    --type title

if [[ ! -f "$ROOT/build/default.xex" ]]; then
    echo "SynthXEX did not create default.xex." >&2
    exit 1
fi

cp "$ROOT/config.ini" "$ROOT/build/config.ini"

echo
echo "Build successful:"
ls -lh "$ROOT/build/default.xex"
