#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party"
mkdir -p "$DEST"

BASE="https://raw.githubusercontent.com/nothings/stb/master"

fetch() {
  local url="$1"
  local out="$2"
  echo "Fetching $out"
  curl --fail --location --retry 3 --silent --show-error "$url" -o "$DEST/$out"
}

fetch "$BASE/stb_image.h" "stb_image.h"
fetch "$BASE/deprecated/stb_image_resize.h" "stb_image_resize.h"
fetch "$BASE/stb_image_write.h" "stb_image_write.h"
fetch "$BASE/LICENSE" "STB-LICENSE.txt"
