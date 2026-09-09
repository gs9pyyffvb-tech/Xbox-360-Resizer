#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/.openxechain"
SRC="$WORK/buildscript"
PREFIX="$WORK/sysroot"

mkdir -p "$WORK"

if [[ ! -d "$SRC/.git" ]]; then
    git clone https://github.com/OpenXeChain/buildscript.git "$SRC"

    git -C "$SRC" config --file .gitmodules --get-regexp url | while read -r key url; do
        https_url="${url/git@github.com:/https:\/\/github.com\/}"
        git -C "$SRC" config --file .gitmodules "$key" "$https_url"
    done

    git -C "$SRC" submodule sync --recursive
    git -C "$SRC" submodule update --init --recursive
else
    git -C "$SRC" pull --ff-only
    git -C "$SRC" submodule sync --recursive
    git -C "$SRC" submodule update --init --recursive
fi

python3 - "$SRC/build-toolchain.sh" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()

marker = "Restored cross compiler found; skipping LLVM rebuild."

if marker not in text:
    start_marker = "# Configure it first"
    end_marker = "rm -rf * # Clear the build directory, ready for xecorelib"

    start = text.index(start_marker)
    end = text.index(end_marker, start) + len(end_marker)

    original = text[start:end]

    replacement = '''if [[ -x "${PREFIX}/bin/clang" && -x "${PREFIX}/bin/lld-link" ]]; then
    echo -e "${TOOLCHAIN_STEM}Restored cross compiler found; skipping LLVM rebuild."
    rm -rf *
else
''' + original + '''
fi'''

    text = text[:start] + replacement + text[end:]
    path.write_text(text)
PY

set +e

PREFIX="$PREFIX" PARALLEL="${PARALLEL:-$(nproc)}" \
    bash "$SRC/build-toolchain.sh"

RESULT=$?

set -e

if [[ "$RESULT" -ne 0 ]]; then
    echo
    echo "============================================================"
    echo "OPENXECHAIN FAILED - BUILD.LOG"
    echo "============================================================"

    if [[ -f "$SRC/build.log" ]]; then
        tail -n 500 "$SRC/build.log"
    else
        echo "build.log was not found."
    fi

    echo "============================================================"
    exit "$RESULT"
fi

echo "OpenXeChain installed to: $PREFIX"
