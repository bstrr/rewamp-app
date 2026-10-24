#!/usr/bin/env bash
# Oracle: rewamp_load_file hors du fil appelant (voir verify_load_busy.c).
# Pendant qu'un chargement LENT tourne sur un fil, toute commande d'un autre fil
# doit rendre REWAMP_ERROR_BUSY (-5), une sonde doit ATTENDRE, et un chargement
# sain doit suivre normalement. Épingle .mod sur UADE pour que le fichier lent
# le soit vraiment (sous libopenmpt il s'ouvre en 10 ms).
#
#   ./verify_load_busy.sh <slow.mod> <good.mod>
#
# <slow.mod>: un module dont l'init de player UADE échoue sur la borne de 60 s
# virtuelles (~0,5 s réelles) — p. ex. un ST2.6 à longueur de morceau 0.
# Exige `cd app && flutter build macos --debug` au préalable (framework Debug).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
fwd="$root/app/build/macos/Build/Products/Debug/rewamp.app/Contents/Frameworks"
[ $# -eq 2 ] || { echo "usage: $0 <slow.mod> <good.mod>" >&2; exit 2; }
[ -d "$fwd/rewamp_audio.framework" ] || { echo "framework Debug absent: cd app && flutter build macos --debug" >&2; exit 2; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cc -O1 -o "$tmp/loadbusy" "$here/verify_load_busy.c" -lpthread
DYLD_FRAMEWORK_PATH="$fwd" DYLD_LIBRARY_PATH="$fwd" "$tmp/loadbusy" \
  "$fwd/rewamp_audio.framework/Versions/A/rewamp_audio" "$root/app/assets" "$1" "$2" \
  2>&1 | grep -v 'stroken\|striken'
