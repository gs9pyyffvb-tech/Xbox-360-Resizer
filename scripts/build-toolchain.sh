#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/.openxechain"
SRC="$WORK/buildscript"
PREFIX="$WORK/sysroot"
BUILD_LOG="$ROOT/build.log"
CONFIG_LOG="$SRC/build/config.log"

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

# ---------------------------------------------------------
# PATCH 1:
# If GitHub restored our previously compiled LLVM/Clang,
# don't spend another 90 minutes rebuilding it.
# ---------------------------------------------------------

skip_marker = "Restored cross compiler found; skipping LLVM rebuild."

if skip_marker not in text:
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


# ---------------------------------------------------------
# PATCH 2:
# OpenXeChain invokes clang-cpp without the Xbox sysroot.
#
# That caused Clang's built-in limits.h to fall through to:
#
#   /usr/include/limits.h
#
# on the Ubuntu GitHub runner, which then failed looking for:
#
#   bits/libc-header-start.h
#
# Explicitly give the preprocessor the Xbox sysroot and
# xecorelib bootstrap headers.
# ---------------------------------------------------------

old_cpp = 'CPP="${PREFIX}/bin/clang-cpp" \\\n'
new_cpp = 'CPP="${PREFIX}/bin/clang-cpp --sysroot=${PREFIX} -I${PWD}/xecorelibtmp/include" \\\n'

if old_cpp in text:
    text = text.replace(old_cpp, new_cpp, 1)
elif "--sysroot=${PREFIX}" not in text:
    raise RuntimeError(
        "Could not locate OpenXeChain Newlib CPP configuration line."
    )

path.write_text(text)
PY

echo "OpenXeChain wrapper patches applied."

set +e

PREFIX="$PREFIX" PARALLEL="${PARALLEL:-$(nproc)}" \
    bash "$SRC/build-toolchain.sh"

RESULT=$?

set -e

if [[ "$RESULT" -ne 0 ]]; then
    echo
    echo "============================================================"
    echo "OPENXECHAIN FAILED"
    echo "============================================================"

    echo
    echo "===== build.log ====="

    if [[ -f "$BUILD_LOG" ]]; then
        tail -n 1200 "$BUILD_LOG"
    else
        echo "build.log not found at: $BUILD_LOG"
    fi

    echo
    echo "===== Newlib config.log ====="

    if [[ -f "$CONFIG_LOG" ]]; then
        tail -n 1500 "$CONFIG_LOG"
    else
        echo "config.log not found at: $CONFIG_LOG"
        echo
        echo "Searching for config.log files:"
        find "$ROOT" -name config.log -type f -print 2>/dev/null || true
    fi

    echo
    echo "============================================================"

    exit "$RESULT"
fi

echo "OpenXeChain installed to: $PREFIX"
