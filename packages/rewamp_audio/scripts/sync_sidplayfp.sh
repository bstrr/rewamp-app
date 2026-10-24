#!/usr/bin/env bash
# Régénère libsidplayfp + libresidfp vendorés (third_party/libsidplayfp/) depuis
# l'amont épinglé + les patchs rewamp. Même philosophie que sync_libxmp.sh.
#
# Jusqu'au 2026-10-22 ces deux arbres étaient des copies faites à la main, sans
# base notée ni patchs séparés: la base a été retrouvée en cherchant le commit
# amont qui minimise les fichiers différents (libsidplayfp 1c4a840, libresidfp
# 9fa6399 — 5 et 4 fichiers, exactement nos modifications).
#
#   third_party/libsidplayfp/<lib>/**            arbre vendoré (ce qui compile)
#   third_party/libsidplayfp/upstream/<lib>/...  copie PRISTINE des fichiers patchés
#   patches/sidplayfp/<lib>/<chemin>.patch       les modifications rewamp
#   patches/sidplayfp/UPSTREAM_VERSION           tags + commits amont épinglés
#
# Usage:
#   ./scripts/sync_sidplayfp.sh                      # ré-applique les patchs
#   ./scripts/sync_sidplayfp.sh <sid-ref> <rfp-ref>  # RE-VENDORE ces références
#                                                    # amont, puis ré-applique
#
# Ce qui n'est PAS repris de l'amont, et pourquoi:
#   - les fichiers que configure GÉNÈRE (config.h, sidversion.h,
#     siddefs-fp.h, sl_defs.h): régénérés ici depuis leurs gabarits .in, avec
#     les valeurs que configure poserait sur nos cibles;
#   - les binaires 6502 (psiddrv.bin, sidplayer1/2.bin): assemblés par xa,
#     absent ici. Ils sont GARDÉS — le script refuse de continuer si leurs
#     sources .a65 ont bougé amont;
#   - les deux sous-modules de pilotes matériels (exsid/usbsid-builder/driver):
#     gardés tels quels, rewamp ne bâtit pas ces builders.
set -euo pipefail

PKG_DIR="$(cd "$(dirname "$0")/.." && pwd)"
V="$PKG_DIR/third_party/libsidplayfp"
UP_DIR="$V/upstream"
PATCH_DIR="$PKG_DIR/patches/sidplayfp"

# Générés ou hors périmètre: ni écrasés ni effacés par le re-vendorage.
KEEP=(--exclude=config.h --exclude=config.h.in --exclude=sidversion.h
      --exclude=siddefs-fp.h --exclude=sl_defs.h --exclude='*.bin'
      --exclude=builders/exsid-builder/driver/ --exclude=builders/usbsid-builder/driver/)

version_of() {  # <dir> → "maj min lev" lus dans configure.ac
  local f="$1/configure.ac"
  printf '%s %s %s\n' \
    "$(sed -n 's/^m4_define(\[lib_major\], \[\(.*\)\])/\1/p' "$f")" \
    "$(sed -n 's/^m4_define(\[lib_minor\], \[\(.*\)\])/\1/p' "$f")" \
    "$(sed -n 's/^m4_define(\[lib_level\], \[\(.*\)\])/\1/p' "$f")"
}

if [ $# -ge 1 ]; then
  [ $# -eq 2 ] || { echo "usage: $0 [<libsidplayfp-ref> <libresidfp-ref>]" >&2; exit 2; }
  SID_REF="$1"; RFP_REF="$2"
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  echo "== clone amont libsidplayfp $SID_REF, libresidfp $RFP_REF"
  git clone --quiet https://github.com/libsidplayfp/libsidplayfp "$TMP/libsidplayfp"
  git clone --quiet https://github.com/libsidplayfp/libresidfp   "$TMP/libresidfp"
  git -C "$TMP/libsidplayfp" checkout --quiet "$SID_REF"
  git -C "$TMP/libresidfp"   checkout --quiet "$RFP_REF"

  # Les binaires 6502 ne sont pas régénérables ici: leurs sources doivent
  # être celles que nos .bin ont assemblées.
  OLD_SID="$(sed -n 2p "$PATCH_DIR/UPSTREAM_VERSION" | awk '{print $2}')"
  if [ -n "$OLD_SID" ] && git -C "$TMP/libsidplayfp" cat-file -e "$OLD_SID" 2>/dev/null; then
    if ! git -C "$TMP/libsidplayfp" diff --quiet "$OLD_SID" HEAD -- '*.a65'; then
      echo "ÉCHEC: les sources 6502 (.a65) ont changé amont — réassembler les .bin (xa) avant." >&2
      exit 1
    fi
  fi

  for lib in libsidplayfp libresidfp; do
    echo "== re-vendoring $lib"
    rsync -a --delete "${KEEP[@]}" --exclude=.git "$TMP/$lib/src/" "$V/$lib/src/"
    for f in NEWS.md README.md AUTHORS AUTHORS.md COPYING LICENSE configure.ac; do
      [ -f "$TMP/$lib/$f" ] && cp "$TMP/$lib/$f" "$V/$lib/$f"
    done
  done

  # Fichiers générés, depuis leurs gabarits.
  read -r SMAJ SMIN SLEV < <(version_of "$V/libsidplayfp")
  read -r RMAJ RMIN RLEV < <(version_of "$V/libresidfp")
  sed -e "s/@LIB_MAJOR@/$SMAJ/" -e "s/@LIB_MINOR@/$SMIN/" -e "s/@LIB_LEVEL@/$SLEV/" \
    "$V/libsidplayfp/src/sidplayfp/sidversion.h.in" > "$V/libsidplayfp/src/sidplayfp/sidversion.h"
  sed -e "s/@LIB_MAJOR@/$RMAJ/" -e "s/@LIB_MINOR@/$RMIN/" -e "s/@LIB_LEVEL@/$RLEV/" \
    "$V/libresidfp/src/residfp/sidversion.h.in" > "$V/libresidfp/src/residfp/sidversion.h"
  sed -e "s/@RESIDFP_BRANCH_HINTS@/1/" -e "s/@HAVE_BUILTIN_EXPECT@/1/" \
      -e "s/@PACKAGE_VERSION@/$RMAJ.$RMIN.$RLEV/" \
      -e "s/@RESIDFP_INLINING@/1/" -e "s/@RESIDFP_INLINE@/inline/" \
    "$V/libresidfp/src/siddefs-fp.h.in" > "$V/libresidfp/src/siddefs-fp.h"
  if grep -q '@[A-Z_]*@' "$V/libresidfp/src/siddefs-fp.h"; then
    echo "ÉCHEC: siddefs-fp.h.in porte une substitution inconnue:" >&2
    grep -n '@[A-Z_]*@' "$V/libresidfp/src/siddefs-fp.h" >&2
    exit 1
  fi
  sed -i.bak -e "s/^#define PACKAGE_VERSION \".*\"/#define PACKAGE_VERSION \"$SMAJ.$SMIN.$SLEV\"/" \
             -e "s/^#define VERSION \".*\"/#define VERSION \"$SMAJ.$SMIN.$SLEV\"/" \
             -e "s/^#define PACKAGE_STRING \"libsidplayfp .*\"/#define PACKAGE_STRING \"libsidplayfp $SMAJ.$SMIN.$SLEV\"/" \
    "$V/libsidplayfp/src/config.h"
  rm -f "$V/libsidplayfp/src/config.h.bak"

  # Copies pristines des fichiers que nous patchons.
  rm -rf "$UP_DIR"
  find "$PATCH_DIR" -name '*.patch' | while read -r p; do
    rel="${p#"$PATCH_DIR"/}"; rel="${rel%.patch}"   # <lib>/<chemin>
    lib="${rel%%/*}"; fpath="${rel#*/}"
    mkdir -p "$(dirname "$UP_DIR/$rel")"
    cp "$TMP/$lib/$fpath" "$UP_DIR/$rel"
  done

  # Ligne 2 = « libsidplayfp <commit> <ref> », relue à la resynchro suivante.
  {
    echo "# libsidplayfp / libresidfp vendorés (github.com/libsidplayfp): <lib> <commit> <ref>"
    echo "libsidplayfp $(git -C "$TMP/libsidplayfp" rev-parse HEAD) $SID_REF"
    echo "libresidfp $(git -C "$TMP/libresidfp" rev-parse HEAD) $RFP_REF"
  } > "$PATCH_DIR/UPSTREAM_VERSION"
fi

echo "== application des patchs"
fail=0
while read -r p; do
  rel="${p#"$PATCH_DIR"/}"; rel="${rel%.patch}"
  lib="${rel%%/*}"; fpath="${rel#*/}"
  cp "$UP_DIR/$rel" "$V/$lib/$fpath"
  if patch -p1 -d "$V/$lib" --quiet --no-backup-if-mismatch < "$p"; then
    echo "   $rel"
  else
    echo "ÉCHEC: $rel ne s'applique pas sur l'amont" >&2
    fail=1
  fi
done < <(find "$PATCH_DIR" -name '*.patch' | sort)
[ "$fail" -eq 0 ] || {
  cat >&2 <<'MSG'

Porter la modification à la main dans third_party/libsidplayfp/<lib>/<chemin>,
puis régénérer le patch depuis la copie pristine:
  diff -u --label a/<chemin> --label b/<chemin> \
    third_party/libsidplayfp/upstream/<lib>/<chemin> \
    third_party/libsidplayfp/<lib>/<chemin> > patches/sidplayfp/<lib>/<chemin>.patch
MSG
  exit 1
}

cat <<'NOTE'

Rappels APRÈS une resynchro:
  - la liste EXPLICITE des sources (cmake/rewamp.cmake + les DEUX podspecs)
    suit-elle les fichiers ajoutés/retirés amont ?
  - REWAMP_PATCHES.md (third_party/libsidplayfp/) décrit chaque patch;
  - rebâtir et écouter un SID (capture des 4 voies, seek, MD5 HVSC).
NOTE
