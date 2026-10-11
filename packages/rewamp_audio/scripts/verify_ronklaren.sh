#!/usr/bin/env bash
# Harnais ASan/UBSan du lecteur Ron Klaren (src/rewamp_plugin_ronklaren.cpp,
# portage de NostalgicPlayer sous third_party/ronklaren).
#   scripts/verify_ronklaren.sh [secondes] [dossier_wav] -- <fichier.rk>...
# Rend chaque sous-chanson en WAV, imprime length(), RMS global et par voix,
# l'état de l'oscilloscope, l'écart d'un seek et la fin réelle.
# Sortie 3 = une sous-chanson silencieuse, 1 = refus d'un fichier.
set -euo pipefail
pkg="$(cd "$(dirname "$0")/.." && pwd)"
build="${TMPDIR:-/tmp}/verify_ronklaren"
mkdir -p "$build"
secs=30; outdir="$build"
if [ "${1:-}" != "--" ] && [ $# -gt 0 ]; then secs="$1"; shift; fi
if [ "${1:-}" != "--" ] && [ $# -gt 0 ]; then outdir="$1"; shift; fi
[ "${1:-}" = "--" ] && shift
mkdir -p "$outdir"
bin="$build/verify_ronklaren"
srcs=("$pkg/src/rewamp_plugin_ronklaren.cpp" "$pkg/third_party/ronklaren/RonKlarenWorker.cpp"
      "$pkg/scripts/verify_ronklaren.cpp")
deps=("${srcs[@]}" "$pkg/src/rewamp_channel_data.c" "$pkg/third_party/ronklaren/RonKlarenWorker.h"
      "$pkg/third_party/ronklaren/RonKlarenContainers.h")
if [ ! -x "$bin" ] || [ -n "$(find "${deps[@]}" -newer "$bin" 2>/dev/null)" ]; then
  san=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
  cc "${san[@]}" -w -I"$pkg/src" -c "$pkg/src/rewamp_channel_data.c" -o "$build/channel_data.o"
  c++ -std=c++17 "${san[@]}" -Wall -Wextra -DREWAMP_WITH_RONKLAREN=1 -I"$pkg/src" \
      "${srcs[@]}" "$build/channel_data.o" -liconv -lm -o "$bin"
fi
rc=0
for f in "$@"; do "$bin" "$f" "$secs" "$outdir" || rc=$?; done
exit $rc
