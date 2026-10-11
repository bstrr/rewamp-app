#!/usr/bin/env bash
# Guard against an ODR collision on `struct RewampDecoder`.
#
# rewamp_plugin.h only forward-declares RewampDecoder; each plugin defines its
# own, of a different shape. In C that is harmless (struct tags have no
# linkage). In C++ it is fine only as long as the type emits NO symbol: the
# moment a plugin's RewampDecoder gets a non-trivial member (std::vector,
# std::string, a destructor…) the compiler emits `RewampDecoder::~RewampDecoder()`
# as a WEAK symbol, and if a second plugin does the same the linker keeps ONE
# of them — plugin A then frees its decoder with plugin B's destructor.
# Nothing warns; it corrupts the heap at close.
#
# Rule: a C++ plugin whose decoder state is not trivially destructible defines
# its own type in an anonymous namespace (TttDecoder, RkDecoder…) and converts
# at the vtable boundary. This script checks the BUILT objects (the ground
# truth): no object may define any symbol scoped to RewampDecoder.
#
#   packages/rewamp_audio/scripts/check_decoder_odr.sh [objects-dir]
#
# Default dir: the newest macOS pod object directory under app/build. Debug
# objects are the best witness (-O0 inlines nothing, so every used member is
# emitted).
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
dir="${1:-}"
if [ -z "$dir" ]; then
  dir=$(ls -dt "$root"/app/build/macos/Build/Intermediates.noindex/Pods.build/*/rewamp_audio.build/Objects-normal/arm64 \
          2>/dev/null | head -1)
fi
if [ -z "$dir" ] || [ ! -d "$dir" ]; then
  echo "no object directory (build once, or pass one: Linux → app/build/linux/<arch>/<mode>/plugins/rewamp_audio)"
  exit 2
fi

bad=0
count=0
while IFS= read -r -d '' o; do
  count=$((count + 1))
  hits=$(nm "$o" 2>/dev/null | awk '$2 != "U" && $2 != "u" {print $3}' \
         | c++filt | grep -E '(^|[^A-Za-z0-9_])RewampDecoder::|for RewampDecoder$|<RewampDecoder[,>]' || true)
  if [ -n "$hits" ]; then
    echo "ODR RISK in $(basename "$o"):"
    printf '%s\n' "$hits" | sed 's/^/   /'
    bad=1
  fi
done < <(find "$dir" -name '*.o' -print0)

[ "$count" = 0 ] && { echo "no .o under $dir"; exit 2; }
[ "$bad" = 0 ] && echo "OK: $count objects, none defines a RewampDecoder-scoped symbol"
exit "$bad"
