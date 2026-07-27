#!/usr/bin/env bash
# Copyright (C) 2026 Lone Crow Design, LLC
# Licensed under the MIT License. See LICENSE.
#
# Renders the OLED screen carousel to PNGs on the host. No device needed.
#
#   ./tools/screen_render/render.sh [output-dir]
#
# u8g2's C sources come from the PlatformIO library dependency, so run a
# `pio run` for any env at least once first to populate .pio/libdeps.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-$root/working/images/carousel_demo}"

u8g2=""
for d in "$root"/.pio/libdeps/*/U8g2/src/clib; do
  [ -d "$d" ] && u8g2="$d" && break
done
if [ -z "$u8g2" ]; then
  echo "u8g2 clib not found under .pio/libdeps; run 'pio run -e <env>' first" >&2
  exit 1
fi

build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
mkdir -p "$out"

# u8g2's clib includes an mui.c/mui_u8g2.c menu layer the renderer does not use
# and which wants symbols we do not link; the screens draw through u8g2 proper.
srcs=$(find "$u8g2" -name '*.c' ! -name 'mui*.c')

# shellcheck disable=SC2086
gcc -c -O1 -w -I"$u8g2" $srcs
mv ./*.o "$build/"

g++ -O1 -Wall -o "$build/render" "$here/render.cpp" "$build"/*.o \
    -I"$u8g2" -I"$root/src"

"$build/render" "$out" >/dev/null
python3 "$here/to_png.py" "$out"

echo "wrote PNGs to $out"
