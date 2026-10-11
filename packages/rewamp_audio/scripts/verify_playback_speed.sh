#!/usr/bin/env bash
# Oracle de la vitesse NATIVE des greffons (voir verify_playback_speed.c), sur
# le moteur macOS compilé.
#   scripts/verify_playback_speed.sh <rewamp_audio.framework/…/rewamp_audio> <data_dir> <vitesse> fichiers…
# Le framework: celui de l'app (app/build/macos/…) ou d'un build de la cible
# pod seule. Le data_dir: celui de l'app (…/rewamp_data), pour UADE & co.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
fw="$1"; shift
fwdir="$(cd "$(dirname "$fw")/../../.." && pwd)"   # …/Frameworks (libEGL & co)
bin="${TMPDIR:-/tmp}/verify_playback_speed"
cc -O1 -w -I"$here/../src" -Wl,-rpath,"$fwdir" \
   -Wl,-rpath,"$here/../../../app/build/macos/Build/Products/Debug/rewamp.app/Contents/Frameworks" \
   -o "$bin" "$here/verify_playback_speed.c"
exec "$bin" "$fw" "$@"
