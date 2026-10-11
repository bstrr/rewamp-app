#!/usr/bin/env bash
# Harnais du greffon SAM Coupé COP (src/rewamp_plugin_samcop.cpp): le code Z80
# du fichier tourne dans z80ex, le son sort de la SAA1099 de zxtune.
#   scripts/verify_samcop.sh [secondes] [dossier_wav] -- <fichier.cop>...
# Rend chaque sous-chanson en WAV, imprime length(), RMS global et par voix,
# l'état de l'oscilloscope, l'écart d'un seek, la fin réelle et une boucle.
# Sortie 3 = une sous-chanson silencieuse, 1 = refus d'un fichier.
#
# zxtune (jeu curaté, ~240 fichiers) est compilé UNE fois en archive, sans
# sanitizer; le greffon et le harnais le sont sous ASan/UBSan.
set -euo pipefail
pkg="$(cd "$(dirname "$0")/.." && pwd)"
zx="$pkg/third_party/libzxtune"
build="${TMPDIR:-/tmp}/verify_samcop"
mkdir -p "$build/zx"
secs=30; outdir="$build"
if [ "${1:-}" != "--" ] && [ $# -gt 0 ]; then secs="$1"; shift; fi
if [ "${1:-}" != "--" ] && [ $# -gt 0 ]; then outdir="$1"; shift; fi
[ "${1:-}" = "--" ] && shift
mkdir -p "$outdir"

defs=(-DREWAMP_WITH_ZXTUNE=1 -DBOOST_ERROR_CODE_HEADER_ONLY -DMODIZER
      -DBOOST_NO_RTTI -DBOOST_SYSTEM_NO_DEPRECATED -DNO_DEBUG_LOGS -DNO_L10N
      -DWORDS_LITTLE_ENDIAN -DZ80EX_API_REVISION=1 -DZ80EX_VERSION_MAJOR=1
      -DZ80EX_VERSION_MINOR=19 -DZ80EX_RELEASE_TYPE=pre1 -DZ80EX_VERSION_STR=1.1.19pre1)
incs=(-I"$zx/include" -I"$zx/src" -I"$zx/3rdparty" -I"$zx" -I"$zx/3rdparty/z80ex/include"
      -I"$pkg/src")

lib="$build/libzx.a"
if [ ! -f "$lib" ]; then
  echo "compilation de zxtune (une fois)…" >&2
  srcs=$(grep -oE '\$\{ZXTUNE_ROOT\}/[^"]+\.(c|cc|cpp)' "$zx/zxtune_sources.cmake" |
         sed "s#^\${ZXTUNE_ROOT}#$zx#")
  printf '%s\n' $srcs "$pkg/src/rewamp_zxtune_stubs.cpp" | xargs -P 8 -I{} sh -c '
    s="$1"; shift; o="'"$build"'/zx/$(echo "$s" | tr "/." "__").o"
    case "$s" in
      *.c) cc -std=gnu11 -O1 -w "$@" -c "$s" -o "$o" ;;
      *)   c++ -std=c++14 -O1 -w "$@" -c "$s" -o "$o" ;;
    esac' _ {} "${defs[@]}" "${incs[@]}"
  ar rcs "$lib" "$build"/zx/*.o
fi

bin="$build/verify_samcop"
deps=("$pkg/src/rewamp_plugin_samcop.cpp" "$pkg/scripts/verify_samcop.cpp" "$pkg/src/rewamp_channel_data.c")
if [ ! -x "$bin" ] || [ -n "$(find "${deps[@]}" -newer "$bin" 2>/dev/null)" ]; then
  san=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
  cc "${san[@]}" -w -I"$pkg/src" -c "$pkg/src/rewamp_channel_data.c" -o "$build/channel_data.o"
  c++ -std=c++14 "${san[@]}" -Wall -Wextra -Wno-unused-parameter "${defs[@]}" "${incs[@]}" \
      "$pkg/src/rewamp_plugin_samcop.cpp" "$pkg/scripts/verify_samcop.cpp" \
      "$build/channel_data.o" "$lib" -lz -liconv -lm -o "$bin"
fi
rc=0
for f in "$@"; do "$bin" "$f" "$secs" "$outdir" || rc=$?; done
exit $rc
