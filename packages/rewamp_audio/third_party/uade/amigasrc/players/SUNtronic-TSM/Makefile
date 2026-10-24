# vasm (http://sun.hasenbraten.de/vasm/), built with CPU=m68k SYNTAX=mot.
# The output is the binary vendored in data/players/ (then rsync'ed to
# app/assets/uade/ and _kBundledAssetsVersion bumped).
VASM ?= vasmm68k_mot

../../../data/players/SUNtronic-TSM: SUNtronic-TSM.s
	$(VASM) -Fhunkexe -nosym -quiet -o $@ $<
