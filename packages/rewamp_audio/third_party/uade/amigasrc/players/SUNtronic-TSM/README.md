# SUNtronic-TSM

EaglePlayer written for Rewamp (2026-10-15): the third SUNtronic replay variant,
which Wanted Team's `SUN-Tronic` refuses (`module check failed`). Only the two
TSM rips on modland (`SunTronic/TSM/pseudo oops.sun`, `unknown.sun`) use it;
the five others stay on `SUN-Tronic`. Detection is by content (`amifilemagic.c`
→ prefix `sun_tsm`), since the extension is the same.

Build: `make VASM=/path/to/vasmm68k_mot` (vasm 2.x, `CPU=m68k SYNTAX=mot`),
then `rsync` `data/` → `app/assets/uade/` and bump `_kBundledAssetsVersion`.

Test without the app (Homebrew `uade` provides `uade123`):

    uade123 -t 20 -e wav -f out.wav -P data/players/SUNtronic-TSM "pseudo oops.sun"

Expected: `Player name: SUNtronic-TSM`, a WAV with RMS ≈ 0.06–0.12 per second;
`shades.sun` (first variant) must still be refused by this player's Check2.
