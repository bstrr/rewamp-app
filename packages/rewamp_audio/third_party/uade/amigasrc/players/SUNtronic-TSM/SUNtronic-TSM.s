; SUNtronic-TSM — EaglePlayer for the third SUNtronic replay variant
; (the two TSM rips on modland: "pseudo oops.sun" and "unknown.sun").
;
; A .sun module is the ripped 68k replay of a demo plus its data. Wanted
; Team's SUN-Tronic player recognises two layouts of the replay's variable
; block ("tst.b $18(a6)" / "tst.b $10(a6)" at module+8), relocates the
; ABSOLUTE pointers those rips carry, and synthesises the init that the rip
; lost. This third variant ("tst.b $7c0(a6)") is position-independent — every
; pointer in its data is an offset from a6 — so nothing needs relocating, but
; the rip lost its init too: the two runtime pointers, voice work area at
; $7c8(a6) and position table at $7d0(a6), are 0 in the file, and the player
; refuses it ("module check failed"). This player only supplies that init.
;
; What the init must do was read from the replay itself (a position change
; loads entry pointers + transposes into the voices) and from the runtime
; state captured inside unknown.sun (voice 0 after a re-init: pattern pointer
; = entry 0, $14(voice) = $ff, transpose = entry 0 byte $10). The initial
; counters (tick 5, row -1, speed 6, master volume $40) are the module's own
; static values at $7bc(a6).
;
; Layout, with a6 = module + 6 + word(module+6) (= module - $ac in both rips):
;   $7bc.b tick     $7bd.b row      $7be.b speed    $7bf.b DMA mask
;   $7c0.b end flag $7c1.b master volume            $7c2-$7c5 fade
;   $7c6.w position $7c8.l voices   $7cc/$7ce.w noise state
;   $7d0.l position table           $7d6.w its a6-relative offset
;   $7dc period table  $86c synth instruments ($24 each)  $8d8 sampled ($1c)
; Position table: entries of 4 pattern offsets + 4 transposes; 0 = restart,
; -1 = end. Voice: $130 bytes — $00.l pattern ptr, $04.l instrument ptr,
; $14.b flags ($ff = idle), $26/$a6 waveform buffers (Paula plays $26, so
; the voices live in CHIP memory), $12e.b transpose.
;
; Build (vasm, http://sun.hasenbraten.de/vasm/): see Makefile.

DTP_PlayerVersion	EQU	$80004458
DTP_PlayerName		EQU	$80004459
DTP_Creator		EQU	$8000445a
DTP_Check2		EQU	$8000445c
DTP_Interrupt		EQU	$8000445e
DTP_InitPlayer		EQU	$80004463
DTP_EndPlayer		EQU	$80004464
DTP_InitSound		EQU	$80004465
DTP_EndSound		EQU	$80004466
EP_PlayerVersion	EQU	$80004560

; DeliTrackerGlobals (a5)
dtg_ChkData		EQU	$24
dtg_ChkSize		EQU	$28
dtg_GetListData		EQU	$38
dtg_AudioAlloc		EQU	$4c
dtg_AudioFree		EQU	$50
dtg_SongEnd		EQU	$5c

VOICE_SIZE		EQU	$130
; Slack after the fourth voice: the replay's waveform double-buffer copy
; (module+$206) runs on an idle voice too and its length comes from the
; instrument, so keep room past the struct rather than trust it.
VOICES_SIZE		EQU	4*VOICE_SIZE+$200

	section	player,code

	moveq	#-1,d0
	rts
	dc.b	"EPPLAYER"
	dc.l	tags
	dc.b	"$VER: SUNtronic-TSM player module V1.0 (15 Oct 2026)",0
	even
tags:
	dc.l	DTP_PlayerVersion,1
	dc.l	EP_PlayerVersion,9
	dc.l	DTP_PlayerName,pname
	dc.l	DTP_Creator,creator
	dc.l	DTP_Check2,check2
	dc.l	DTP_Interrupt,interrupt
	dc.l	DTP_InitPlayer,initplayer
	dc.l	DTP_EndPlayer,endplayer
	dc.l	DTP_InitSound,initsound
	dc.l	DTP_EndSound,endsound
	dc.l	0
pname:	dc.b	"SUNtronic-TSM",0
creator: dc.b	"Felix Schmidt,",10,"TSM variant adapted by Rewamp",0
	even

modptr:	dc.l	0
delibase: dc.l	0

; d1 = a6 - module, exactly as the replay's "lea xxx(pc),a6" computes it.
a6_offset:
	move.w	6(a0),d1
	ext.l	d1
	addq.l	#6,d1
	rts

; DTP_Check2: d0 = 0 when the module is a TSM-variant SUNtronic rip.
check2:
	lea	delibase(pc),a1
	move.l	a5,(a1)
	movea.l	dtg_ChkData(a5),a0
	move.l	dtg_ChkSize(a5),d2
	moveq	#-1,d0
	cmpi.l	#$48e7fffe,(a0)		; movem.l d0-d7/a0-a6,-(sp)
	bne.b	.fail
	cmpi.w	#$4dfa,4(a0)		; lea xxx(pc),a6
	bne.b	.fail
	cmpi.l	#$4a2e07c0,8(a0)	; tst.b $7c0(a6) — this variant only
	bne.b	.fail
	bsr.b	a6_offset
	bpl.b	.fail			; a6 lies BEFORE the module in this variant
	move.l	d1,d3
	addi.l	#$7bc,d3		; variable block inside the file...
	bmi.b	.fail
	move.l	d3,d4
	addi.l	#$1c,d4
	cmp.l	d2,d4
	bcc.b	.fail
	lea	(a0,d3.l),a1
	move.w	$1a(a1),d4		; $7d6(a6): position table offset, inside too...
	ext.l	d4
	add.l	d1,d4
	bmi.b	.fail
	move.l	d4,d5
	addi.l	#$14,d5
	cmp.l	d2,d5
	bcc.b	.fail
	lea	(a0,d4.l),a1
	moveq	#3,d3
.pat:	move.l	(a1)+,d4		; ...and entry 0 names four patterns inside the file
	add.l	d1,d4
	bmi.b	.fail
	cmp.l	d2,d4
	bcc.b	.fail
	dbra	d3,.pat
	moveq	#0,d0
.fail:	rts

; DTP_InitPlayer: remember the module, plant the song-end hooks, allocate audio.
initplayer:
	moveq	#0,d0			; list entry 0 = the module
	movea.l	dtg_GetListData(a5),a0
	jsr	(a0)
	lea	modptr(pc),a1
	move.l	a0,(a1)
; Each hook replaces a 6-byte sequence by "jsr hook.l", only when the exact
; original bytes are there (both known rips share the same replay code).
	cmpi.l	#$1d7c0001,$8c(a0)	; move.b #1,$7c0(a6): table hit -1, song over
	bne.b	.noend
	cmpi.w	#$07c0,$90(a0)
	bne.b	.noend
	lea	hook_end(pc),a1
	move.w	#$4eb9,$8c(a0)
	move.l	a1,$8e(a0)
.noend:
	cmpi.l	#$426e07c6,$a0(a0)	; clr.w $7c6(a6) + bra.b $78: table hit 0, restart
	bne.b	.noloop
	cmpi.w	#$60d2,$a4(a0)
	bne.b	.noloop
	lea	hook_loop(pc),a1
	move.w	#$4eb9,$a0(a0)
	move.l	a1,$a2(a0)
.noloop:
	movea.l	dtg_AudioAlloc(a5),a0
	jmp	(a0)

hook_end:				; returns to module+$92 (DMA off, exit) like the original
	move.b	#1,$7c0(a6)
	bra.b	songend
hook_loop:				; the original branched back to $78: resume there, not at $a6
	clr.w	$7c6(a6)
	subi.l	#$a6-$78,(sp)
songend:
	movem.l	d0-d7/a0-a6,-(sp)
	movea.l	delibase(pc),a5
	movea.l	dtg_SongEnd(a5),a0
	jsr	(a0)
	movem.l	(sp)+,d0-d7/a0-a6
	rts

; DTP_InitSound: the lost init.
initsound:
	movea.l	modptr(pc),a0
	bsr.w	a6_offset
	lea	(a0,d1.l),a6		; a6 as the replay sees it
	move.b	#5,$7bc(a6)		; tick: the first frame reaches the speed...
	move.b	#-1,$7bd(a6)		; ...and row -1 becomes 0: row 0 is read at once
	move.b	#6,$7be(a6)		; speed (row 0 of the patterns sets the real one)
	clr.b	$7bf(a6)		; DMA mask
	clr.b	$7c0(a6)		; end flag
	move.b	#$40,$7c1(a6)		; master volume
	clr.l	$7c2(a6)		; fade step, reload, counter
	clr.w	$7c6(a6)		; position
	clr.l	$7cc(a6)		; noise generator state
	move.w	$7d6(a6),d0
	ext.l	d0
	lea	(a6,d0.l),a1		; position table
	move.l	a1,$7d0(a6)
	lea	voices,a0
	move.l	a0,$7c8(a6)
	move.w	#VOICES_SIZE/4-1,d0
.clr:	clr.l	(a0)+
	dbra	d0,.clr
	lea	voices,a0
	lea	$86c(a6),a2		; synth instrument 0: a real instrument behind an idle voice
	moveq	#3,d1
.ptr:	move.l	(a1)+,d0		; entry 0: four pattern offsets...
	lea	(a6,d0.l),a3
	move.l	a3,(a0)
	move.l	a2,4(a0)
	move.b	#-1,$14(a0)		; idle until the pattern names an instrument
	lea	VOICE_SIZE(a0),a0
	dbra	d1,.ptr
	lea	voices,a0
	moveq	#3,d1
.tr:	move.b	(a1)+,$12e(a0)		; ...then four transposes
	lea	VOICE_SIZE(a0),a0
	dbra	d1,.tr
	rts

; DTP_Interrupt: one frame of the replay.
interrupt:
	movem.l	d1-d7/a0-a6,-(sp)
	movea.l	modptr(pc),a0
	jsr	(a0)
	movem.l	(sp)+,d1-d7/a0-a6
	moveq	#0,d0
	rts

endplayer:
	movea.l	dtg_AudioFree(a5),a0
	jmp	(a0)

endsound:
	rts

	section	voicebuf,bss_c
voices:	ds.b	VOICES_SIZE
