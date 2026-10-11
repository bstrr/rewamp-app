#!/usr/bin/env bash
# Harnais ASan du lecteur NoiseTracker GS (src/rewamp_plugin_ntgs.c).
#   scripts/verify_ntgs.sh <module> [secondes] [sortie.wav]
#   scripts/verify_ntgs.sh --oracle <module> [secondes]
#       compare les voix B, échantillon par échantillon, à es5503.c de libvgm
# Sortie 3 = rendu silencieux, 1 = ouverture refusée, 4 = l'oracle diverge.
set -euo pipefail
pkg="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/verify_ntgs"
mkdir -p "$out"
flags=(-std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wno-unused-function
       -DREWAMP_WITH_NTGS=1 -I"$pkg/src")
if [ "${1:-}" = "--oracle" ]; then
  shift
  bin="$out/verify_ntgs_oracle"
  srcs=("$pkg/src/rewamp_plugin_ntgs.c" "$pkg/src/rewamp_channel_data.c" "$pkg/scripts/verify_ntgs.c")
  if [ ! -x "$bin" ] || [ -n "$(find "${srcs[@]}" -newer "$bin" 2>/dev/null)" ]; then
    lv="$pkg/third_party/libvgm/libvgm"
    cc "${flags[@]}" -w -DNTGS_ORACLE=1 -I"$lv" -I"$lv/emu" "$pkg/src/rewamp_channel_data.c" \
       "$pkg/scripts/verify_ntgs.c" -liconv -lm -o "$bin"
  fi
else
  bin="$out/verify_ntgs"
  srcs=("$pkg/src/rewamp_plugin_ntgs.c" "$pkg/src/rewamp_channel_data.c" "$pkg/scripts/verify_ntgs.c")
  if [ ! -x "$bin" ] || [ -n "$(find "${srcs[@]}" -newer "$bin" 2>/dev/null)" ]; then
    cc "${flags[@]}" "${srcs[@]}" -liconv -lm -o "$bin"
  fi
fi
"$bin" "$@"
