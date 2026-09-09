#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/.openxechain"
SRC="$WORK/buildscript"
PREFIX="$WORK/sysroot"

mkdir -p "$WORK"

if [[ ! -d "$SRC/.git" ]]; then
  git clone https://github.com/OpenXeChain/buildscript.git "$SRC"
  # OpenXeChain's current .gitmodules uses SSH GitHub URLs. Rewrite them so
  # unattended GitHub Actions runners can fetch the public submodules.
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

PREFIX="$PREFIX" PARALLEL="${PARALLEL:-$(nproc)}" bash "$SRC/build-toolchain.sh"

echo "OpenXeChain installed to: $PREFIX"
