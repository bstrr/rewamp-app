/* NoiseTracker GS 1.x (FTA, Olivier Goguel, Apple IIgs, 1991-92) — lecteur
 * écrit pour rewamp à partir du désassemblage du programme original.
 *
 * Spécification complète (adresses du code 65816 citées): docs/re/noisetracker_gs/
 * NTGS_SPEC.md. Résumé de ce qui compte ici:
 *
 *  - Un module « FTA MODULEFILE » = en-tête, table des voix, liste d'ordre,
 *    motifs (64 lignes × nch canaux × 3 octets), instruments de 32 octets, puis
 *    les données d'échantillons alignées sur une page: bloc A (« RAM based »,
 *    streamé par le programme) puis bloc B (chargé une fois dans la RAM son du
 *    DOC Ensoniq 5503).
 *  - UNE ligne par interruption du timer (oscillateur 30): pas de « speed », pas
 *    d'effets continus. Rythme: une ligne toutes les 130560/T échantillons du
 *    DOC (27118,48 Hz), T = tempo.
 *  - Voix B: une paire d'oscillateurs DOC sur la même table. Elle est émulée ici
 *    par un mini-DOC (accumulateur, taille/résolution, octet $00 = arrêt, fin de
 *    table, mode swap = boucle, la règle de MAME/libvgm `es5503_halt_osc`), sur
 *    une image de 64 Ko de RAM son construite aux pages $17 du fichier.
 *  - Voix A: l'original streame l'échantillon par demi-anneaux de N pages dans
 *    la RAM son; on lit directement le fichier et on reproduit ce qui S'ENTEND:
 *    lecture par blocs de N pages, $00 hors du dernier bloc = saut au bloc
 *    suivant, boucle à la page près.
 *  - On mixe directement à 44100 Hz: le pas d'un oscillateur est sa fréquence
 *    DOC × 27118,48/44100, sans interpolation (le DOC n'en fait pas).
 *
 * Boucle: configure_loop = NULL (chemin générique Dart). Un morceau dont le
 * drapeau d'en-tête $12 vaut 0 S'ARRÊTE (read() rend 0, comme l'original qui
 * coupe tous les oscillateurs); un morceau à $12 ≠ 0 reboucle indéfiniment sur
 * l'ordre 0 et length() rend la durée d'UN passage.
 *
 * Compilé seulement si REWAMP_WITH_NTGS est défini. */
#ifdef REWAMP_WITH_NTGS

#include "rewamp_plugin.h"
#include "rewamp_channel_data.h"
#include "ModizerVoicesData.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTGS_RATE        44100
/* Fréquence utilisée pour les PAS et le rythme. Le harnais oracle la remplace
 * par celle du DOC (verify_ntgs.sh --oracle) pour comparer échantillon par
 * échantillon avec es5503.c de libvgm. */
#ifndef NTGS_STEP_RATE
#define NTGS_STEP_RATE   44100.0
#endif
/* Écritures de registres DOC des voix B, pour le même oracle (vide sinon). */
#ifndef NTGS_DOCW
#define NTGS_DOCW(d, s, k, reg, val) ((void)0)
#endif
#define NTGS_DOC_RATE    (7159090.0 / 8.0 / 33.0)   /* 31 oscillateurs actifs */
#define NTGS_CHUNK       1024
#define NTGS_RING_SIZE   (SOUND_BUFFER_SIZE_SAMPLE * 4 * 2)
#define NTGS_MAX_SLOTS   22
#define NTGS_MAX_FILE    (64u * 1024u * 1024u)

/* Canal de sortie DOC → côté stéréo. Convention MAME/libvgm (`outputs[chan&1]`):
 * canal pair = GAUCHE. C'est le SEUL endroit à changer pour inverser. */
#define NTGS_SIDE_OF_CHAN(c)   ((c) & 1)          /* 0 = gauche, 1 = droite */

/* Gain du mixage: une paire d'oscillateurs à plein volume (2 × 127 × 190)
 * donne ≈ 0,25 de pleine échelle. Certains modules empilent jusqu'à 12 voix sur
 * la même note (057: −5 dBFS RMS au gain précédent): au-delà de 0,75 on écrête
 * en douceur plutôt que de tronquer. */
#define NTGS_GAIN (1.0f / 196608.0f)
#define NTGS_KNEE 0.75f

/* ── Tables du programme original ──────────────────────────────────────── */

/* $AFAA: volume de cellule (0..255) >> 1 → registre de volume du DOC. */
static const uint8_t kVol[128] = {
    0x00,0x02,0x04,0x05,0x06,0x07,0x09,0x0a,0x0c,0x0d,0x0f,0x10,0x12,0x13,0x15,0x16,
    0x18,0x19,0x1b,0x1c,0x1e,0x1f,0x21,0x22,0x24,0x25,0x27,0x28,0x2a,0x2b,0x2d,0x2e,
    0x30,0x31,0x33,0x34,0x36,0x37,0x39,0x3a,0x3c,0x3d,0x3f,0x40,0x42,0x43,0x45,0x46,
    0x48,0x49,0x4b,0x4c,0x4e,0x4f,0x51,0x52,0x54,0x55,0x57,0x58,0x5a,0x5b,0x5d,0x5e,
    0x60,0x61,0x63,0x64,0x66,0x67,0x69,0x6a,0x6c,0x6d,0x6f,0x70,0x72,0x73,0x75,0x76,
    0x78,0x79,0x7b,0x7c,0x7e,0x7f,0x81,0x82,0x84,0x85,0x87,0x88,0x8a,0x8b,0x8d,0x8e,
    0x90,0x91,0x93,0x94,0x96,0x97,0x99,0x9a,0x9c,0x9d,0x9f,0xa0,0xa2,0xa3,0xa5,0xa6,
    0xa8,0xa9,0xab,0xac,0xae,0xaf,0xb1,0xb2,0xb4,0xb5,0xb7,0xb8,0xba,0xbb,0xbd,0xbe,
};

/* $6FB7: note (index 0..$7F, octet 0 & $7F) → registre de fréquence du DOC.
 * 107 valeurs réelles ($00..$6A) puis les octets CONSTANTS qui suivent la table
 * dans le programme (données graphiques): le code lit là pour une note
 * invalide (vu: $62, $7F), on reproduit. $54/$55 sont des commandes. */
static const uint16_t kFreq[128] = {
    0,22,23,24,26,27,29,30,32,34,36,38,41,43,46,49,51,54,58,61,65,69,73,77,82,86,
    92,97,103,109,115,122,129,137,145,154,163,173,183,194,206,217,230,244,258,274,
    290,307,326,346,367,388,411,436,462,489,518,549,582,617,653,692,733,777,823,
    872,924,979,1037,1098,1164,1233,1306,1384,1466,1553,1646,1744,1847,1957,2074,
    2197,2328,2466,2613,2768,2933,3107,3292,3487,3695,3915,4147,4394,4655,4932,
    5225,5536,5865,6214,6583,6975,7390,7829,8294,8788,9310,
    2688,49407,65280,4095,16380,15612,3855,12540,3843,15612,3855,4095,16380,
    49407,65280,49407,65280,4095,16380,16380,4095,
};

/* Masques du pointeur de table selon la taille (MAME wavemasks, 16 bits). */
static const uint16_t kWaveMask[8] = {
    0xff00, 0xfe00, 0xfc00, 0xf800, 0xf000, 0xe000, 0xc000, 0x8000
};

/* ── Structures ─────────────────────────────────────────────────────────── */

typedef struct {
    char     name[17];
    uint32_t sizePages;     /* $11 */
    uint32_t srcPage;       /* $13 (bit 15: B partagé avec le bloc A) */
    uint8_t  volDef;        /* $15 */
    uint8_t  volCur;        /* $16 — volume COURANT, partagé par toutes les voix */
    uint8_t  docPage;       /* $17 (B) */
    uint8_t  wsr0, ctl0;    /* $19, $1A (B) */
    uint8_t  wsr1, ctl1;    /* $1B, $1C (B) */
    uint8_t  chan0, chan1;  /* $1D, $1E */
    uint16_t loop;          /* $19 mot (A) */
    uint16_t loopStart;     /* $1B mot (A) */
} NtgsInstr;

/* Un oscillateur DOC (voix B). */
typedef struct {
    uint32_t acc;    /* accumulateur DOC, entier 32 bits qui REBOUCLE comme dans
                      * es5503.c (MAME/libvgm), sur lequel l'oracle est calé */
    uint32_t frac;   /* fraction (16 bits) due au pas à 44100 Hz */
    uint32_t inc;    /* fréquence DOC × 27118,48/44100, 16 bits de fraction */
    uint16_t freq;
    uint8_t  vol;    /* registre de volume */
    uint8_t  ctl;    /* registre $A0: bit0 halt, bits1-2 mode, bits4-7 canal */
    uint8_t  ptr;    /* registre $80: page de table */
    uint8_t  wsr;    /* registre $C0: taille (bits 3-5) / résolution (0-2) */
} NtgsOsc;

/* Une voix A (streamée dans l'original). */
typedef struct {
    int      active;
    uint32_t start;       /* octet de départ dans le bloc A */
    uint32_t len;         /* longueur en octets ($11 pages) */
    int      looped;
    uint32_t loopStart;   /* octet de reprise, relatif à start */
    uint32_t blockBytes;  /* N pages × 256 */
    int32_t  finalBlock;  /* dernier bloc (non bouclé) */
    uint32_t block;       /* bloc courant */
    uint64_t offs;        /* position dans le bloc, 32.32 */
    uint64_t inc;         /* pas, 32.32 */
    uint8_t  vol;
    uint8_t  sideLo, sideHi;
} NtgsStream;

typedef struct {
    int        used;     /* slot présent dans le fichier */
    int        isA;
    int        voice;    /* index de voix rewamp (scope, mute) */
    int        lastInstr;/* 1-based, 0 = aucun ($A55A) */
    uint16_t   word;     /* mot de la table des voix (canaux) */
    NtgsOsc    osc[2];   /* voix B */
    NtgsStream st;       /* voix A */
    int        note;     /* dernière note (Hz affichés) */
} NtgsSlot;

struct RewampDecoder {
    uint8_t*   file;
    size_t     fileSize;
    /* en-tête */
    int        tempoHdr, loopFlag, songLen, npat, nWave, nRam, streamCfg;
    int        wavePages, ramPages, nA, nch;
    uint16_t   order[256];
    uint16_t   voiceWord[NTGS_MAX_SLOTS];
    size_t     patOfs;            /* début des motifs */
    size_t     ramOfs, waveOfs;   /* blocs de données (peuvent dépasser le fichier) */
    NtgsInstr* instrA;            /* nRam */
    NtgsInstr* instrB;            /* nWave */
    uint8_t    docRam[65536];
    int        nVoices;
    /* état de lecture */
    NtgsSlot   slot[NTGS_MAX_SLOTS];
    int        orderPos, row, tempo, ended;
    double     nextRow;           /* instant de la prochaine ligne (échantillons) */
    double     speed;             /* vitesse native: lignes ÷ vitesse, le DOC
                                   * garde son horloge (hauteur conservée) */
    uint64_t   cur;               /* échantillons produits */
    uint64_t   rowsPlayed;
    uint64_t   lengthFrames;
    uint64_t   passRows;
    /* tampons de mixage */
    int32_t    mixL[NTGS_CHUNK], mixR[NTGS_CHUNK], vmix[NTGS_CHUNK];
};

/* ── Utilitaires ────────────────────────────────────────────────────────── */

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int ntgs_probe(const char* ext, const uint8_t* header, size_t headerSize) {
    (void)ext;   /* les rips n'ont pas d'extension, ou une fausse: contenu seul */
    if (header && headerSize >= 16 && memcmp(header + 2, "FTA MODULEFILE", 14) == 0)
        return 95;
    return 0;
}

static double ntgs_row_len(int tempo) {
    /* 130560/T échantillons DOC, convertis en échantillons de sortie. */
    return 130560.0 * NTGS_STEP_RATE / ((double)tempo * NTGS_DOC_RATE);
}

static double ntgs_note_hz(uint16_t f) {
    /* Repère d'affichage: la note 37 (F = 145) = do 4 (261,63 Hz). */
    return 261.6256 * (double)f / 145.0;
}

static uint32_t ntgs_doc_inc16(uint16_t f) {
    return (uint32_t)((double)f * NTGS_DOC_RATE / NTGS_STEP_RATE * 65536.0 + 0.5);
}

static const uint8_t* ntgs_cell(RewampDecoder* d, int pat, int row, int chan) {
    if (pat < 0 || pat >= d->npat) return NULL;
    size_t o = d->patOfs + (((size_t)pat * 64 + (size_t)row) * (size_t)d->nch + (size_t)chan) * 3;
    if (o + 3 > d->fileSize) return NULL;
    return d->file + o;
}

static uint8_t ntgs_ram_byte(RewampDecoder* d, uint64_t ofs) {
    /* bloc A, lecture bornée (rip tronqué ⇒ 0) */
    uint64_t o = (uint64_t)d->ramOfs + ofs;
    if (ofs >= (uint64_t)d->ramPages * 256u || o >= d->fileSize) return 0;
    return d->file[o];
}

/* ── Mini-DOC (voix B): règles de es5503_halt_osc ───────────────────────── */

static void ntgs_halt(NtgsOsc* o, NtgsOsc* partner, int isEven, int type) {
    int mode = (o->ctl >> 1) & 3;
    const int pmode = (partner->ctl >> 1) & 3;
    const int size = (o->wsr >> 3) & 7, res = o->wsr & 7;
    const int resshift = 9 + res - size;
    const uint32_t back = ((256u << size) - 1) << resshift;
    if (mode == 2) mode = 0;   /* sync/AM: boucle (la synchro entre paires n'est pas utilisée) */
    if (mode != 0 || type != 0) o->ctl |= 1;
    else o->acc -= back;   /* garde la phase; peut reboucler (uint32), comme MAME */
    if (mode == 3) {
        partner->ctl &= (uint8_t)~1;
        partner->acc = 0;
        partner->frac = 0;
    } else if (pmode == 3 && isEven) {
        /* osc pair pas en swap, partenaire en swap: re-déclenchement (vérifié
         * sur IIgs selon MAME) */
        o->acc -= back;
        o->ctl &= (uint8_t)~1;
    }
}

/* Un échantillon de sortie d'un oscillateur; rend sa contribution. */
static inline int32_t ntgs_osc_tick(RewampDecoder* d, NtgsOsc* o, NtgsOsc* partner, int isEven) {
    if (o->ctl & 1) return 0;
    const int size = (o->wsr >> 3) & 7, res = o->wsr & 7;
    const uint32_t wtsize = 256u << size;
    const int resshift = 9 + res - size;
    const uint32_t sizemask = wtsize - 1;
    const uint32_t wtptr = ((uint32_t)o->ptr << 8) & kWaveMask[size];
    const uint32_t altram = o->acc >> resshift;
    const uint32_t ramptr = altram & sizemask;
    {
        const uint32_t f = o->frac + (o->inc & 0xffffu);
        o->acc += (o->inc >> 16) + (f >> 16);
        o->frac = f & 0xffffu;
    }
    const uint8_t data = d->docRam[(wtptr + ramptr) & 0xffff];
    if (data == 0) { ntgs_halt(o, partner, isEven, 1); return 0; }
    const int32_t out = ((int32_t)data - 128) * (int32_t)o->vol;
    if (altram >= wtsize - 1) ntgs_halt(o, partner, isEven, 0);
    return out;
}

/* ── Voix A: comportement audible du streaming ──────────────────────────── */

static inline uint8_t ntgs_stream_byte(RewampDecoder* d, NtgsStream* s, uint64_t t) {
    if (t < s->len) return ntgs_ram_byte(d, (uint64_t)s->start + t);
    if (!s->looped) return 0;
    const uint64_t loopLen = s->len - s->loopStart;
    return ntgs_ram_byte(d, (uint64_t)s->start + s->loopStart + (t - s->len) % loopLen);
}

static inline int32_t ntgs_stream_tick(RewampDecoder* d, NtgsStream* s) {
    if (!s->active) return 0;
    const uint32_t B = s->blockBytes;
    const uint32_t oi = (uint32_t)(s->offs >> 32);
    const uint64_t t = (uint64_t)s->block * B + (oi & (B - 1));
    const int isFinal = !s->looped && (int32_t)s->block >= s->finalBlock;
    if (!s->looped && t >= s->len) { s->active = 0; return 0; }   /* fin des données */
    const uint8_t data = ntgs_stream_byte(d, s, t);
    if (data == 0) {
        /* $00: arrêt dans le dernier bloc, sinon le swap démarre le bloc suivant */
        if (isFinal) s->active = 0;
        else { s->block++; s->offs = 0; }
        return 0;
    }
    const int32_t out = ((int32_t)data - 128) * (int32_t)s->vol;
    if (oi >= B - 1) {   /* fin de table du demi-anneau */
        if (isFinal) s->active = 0;
        else { s->block++; s->offs = 0; }
    } else {
        s->offs += s->inc;
    }
    return out;
}

static inline float ntgs_soft_clip(float x) {
    const float a = fabsf(x);
    if (a <= NTGS_KNEE) return x;
    const float y = NTGS_KNEE + (1.f - NTGS_KNEE) * tanhf((a - NTGS_KNEE) / (1.f - NTGS_KNEE));
    return x < 0 ? -y : y;
}

/* ── Mixage d'une tranche (out NULL = avancer sans produire) ────────────── */

static void ntgs_mix(RewampDecoder* d, float* out, int n, int capture) {
    memset(d->mixL, 0, sizeof(int32_t) * (size_t)n);
    memset(d->mixR, 0, sizeof(int32_t) * (size_t)n);
    for (int sl = 0; sl < NTGS_MAX_SLOTS; sl++) {
        NtgsSlot* s = &d->slot[sl];
        if (!s->used) continue;
        const int v = s->voice;
        const int muted = (generic_mute_mask >> v) & 1;
        if (s->isA) {
            NtgsStream* st = &s->st;
            if (!st->active) {
                if (capture) memset(d->vmix, 0, sizeof(int32_t) * (size_t)n);
            } else {
                const int sl0 = st->sideLo, sh = st->sideHi;
                for (int i = 0; i < n; i++) {
                    const int32_t o = ntgs_stream_tick(d, st);
                    d->vmix[i] = 2 * o;
                    if (!muted) {
                        if (sl0) d->mixR[i] += o; else d->mixL[i] += o;
                        if (sh) d->mixR[i] += o; else d->mixL[i] += o;
                    }
                }
            }
        } else {
            NtgsOsc* a = &s->osc[0];
            NtgsOsc* b = &s->osc[1];
            if ((a->ctl & 1) && (b->ctl & 1)) {
                if (capture) memset(d->vmix, 0, sizeof(int32_t) * (size_t)n);
            } else {
                for (int i = 0; i < n; i++) {
                    /* ordre du DOC: l'osc pair puis l'impair dans le même échantillon */
                    const int32_t oa = ntgs_osc_tick(d, a, b, 1);
                    const int32_t ob = ntgs_osc_tick(d, b, a, 0);
                    d->vmix[i] = oa + ob;
                    if (!muted) {
                        if (NTGS_SIDE_OF_CHAN(a->ctl >> 4)) d->mixR[i] += oa; else d->mixL[i] += oa;
                        if (NTGS_SIDE_OF_CHAN(b->ctl >> 4)) d->mixR[i] += ob; else d->mixL[i] += ob;
                    }
                }
            }
        }
        if (capture) {
            if (m_voice_buff[v]) {
                for (int i = 0; i < n; i++) {
                    int32_t sc = muted ? 0 : d->vmix[i] / 384;
                    m_voice_buff[v][((m_voice_current_ptr[v] >> MODIZER_OSCILLO_OFFSET_FIXEDPOINT)
                                     + i) & (NTGS_RING_SIZE - 1)] = (signed char)LIMIT8(sc);
                }
            }
            m_voice_current_ptr[v] += (int64_t)n << MODIZER_OSCILLO_OFFSET_FIXEDPOINT;
        }
    }
    if (out) {
        for (int i = 0; i < n; i++) {
            out[2 * i]     = ntgs_soft_clip((float)d->mixL[i] * NTGS_GAIN);
            out[2 * i + 1] = ntgs_soft_clip((float)d->mixR[i] * NTGS_GAIN);
        }
    }
}

/* ── Séquenceur ─────────────────────────────────────────────────────────── */

static int ntgs_slot_of_chan(const RewampDecoder* d, int c) {
    return c < d->nA ? c : c - d->nA + 7;
}

static void ntgs_reset_volumes(RewampDecoder* d) {   /* $A1FD */
    for (int i = 0; i < d->nRam; i++) d->instrA[i].volCur = d->instrA[i].volDef;
    for (int i = 0; i < d->nWave; i++) d->instrB[i].volCur = d->instrB[i].volDef;
}

static void ntgs_halt_all(RewampDecoder* d) {        /* $A912 */
    for (int sl = 0; sl < NTGS_MAX_SLOTS; sl++) {
        for (int k = 0; k < 2; k++) {
            d->slot[sl].osc[k].ctl = 3;       /* $A912 écrit 3 */
            if (d->slot[sl].used && !d->slot[sl].isA) NTGS_DOCW(d, &d->slot[sl], k, 0xa0, 3);
        }
        d->slot[sl].st.active = 0;
    }
}

static void ntgs_set_volume(RewampDecoder* d, NtgsSlot* s, uint8_t vol) {
    (void)d;   /* utilisé par le crochet d'oracle NTGS_DOCW */
    const uint8_t r = kVol[vol >> 1];
    if (s->isA) s->st.vol = r;
    else {
        s->osc[0].vol = r; s->osc[1].vol = r;
        NTGS_DOCW(d, s, 0, 0x40, r); NTGS_DOCW(d, s, 1, 0x40, r);
    }
}

static void ntgs_trigger_b(RewampDecoder* d, NtgsSlot* s, NtgsInstr* in, uint16_t f) {  /* $A840 */
    (void)d;
    const uint8_t vr = kVol[in->volCur >> 1];
    const uint8_t c0 = (in->chan0 & 0x80) ? (uint8_t)(s->word & 0xff) : in->chan0;
    const uint8_t c1 = (in->chan1 & 0x80) ? (uint8_t)(s->word >> 8) : in->chan1;
    const uint8_t m[2]   = { (uint8_t)(c0 | in->ctl0), (uint8_t)(c1 | in->ctl1) };
    const uint8_t wsr[2] = { in->wsr0, in->wsr1 };
    for (int k = 0; k < 2; k++) {
        NtgsOsc* o = &s->osc[k];
        o->ctl = 3;                          /* halt ($A85A écrit 3) */
        o->freq = f;
        o->inc = ntgs_doc_inc16(f);
        o->vol = vr;
        o->ptr = in->docPage;
        o->wsr = wsr[k];
        NTGS_DOCW(d, s, k, 0xa0, 3);
        NTGS_DOCW(d, s, k, 0x00, f & 0xff);
        NTGS_DOCW(d, s, k, 0x20, f >> 8);
        NTGS_DOCW(d, s, k, 0x40, vr);
        NTGS_DOCW(d, s, k, 0x80, in->docPage);
        NTGS_DOCW(d, s, k, 0xc0, wsr[k]);
    }
    for (int k = 0; k < 2; k++) {
        NtgsOsc* o = &s->osc[k];
        if ((o->ctl & 1) && !(m[k] & 1)) { o->acc = 0; o->frac = 0; }   /* départ */
        o->ctl = m[k];
        NTGS_DOCW(d, s, k, 0xa0, m[k]);
    }
}

static void ntgs_trigger_a(RewampDecoder* d, NtgsSlot* s, NtgsInstr* in, uint16_t f) {  /* $A36B/$A684 */
    NtgsStream* st = &s->st;
    const uint16_t w = (uint16_t)(in->chan0 | (in->chan1 << 8));
    const uint16_t chans = (w & 0x8000) ? s->word : w;
    const uint32_t N = d->streamCfg ? 8 : 4;
    memset(st, 0, sizeof(*st));
    st->start = in->srcPage * 256u;
    st->len = in->sizePages * 256u;
    st->looped = in->loop != 0;
    st->loopStart = (uint32_t)in->loopStart * 256u;
    if (st->looped && st->loopStart >= st->len) st->looped = 0;
    st->blockBytes = N * 256u;
    st->finalBlock = (int32_t)((in->sizePages + N - 1) / N) - 1;
    st->inc = (uint64_t)((double)f * NTGS_DOC_RATE / NTGS_STEP_RATE / 512.0 * 4294967296.0 + 0.5);
    st->vol = kVol[in->volCur >> 1];
    st->sideLo = (uint8_t)NTGS_SIDE_OF_CHAN((chans & 0xff) >> 4);
    st->sideHi = (uint8_t)NTGS_SIDE_OF_CHAN((chans >> 8) >> 4);
    st->active = st->len > 0;
}

static void ntgs_end_of_song(RewampDecoder* d) {
    d->ended = 1;
    ntgs_reset_volumes(d);
    ntgs_halt_all(d);
}

/* Charge le motif suivant ($A232). Rend 0 en fin de morceau. */
static int ntgs_next_pattern(RewampDecoder* d) {
    if (d->orderPos >= d->songLen) {
        if (d->loopFlag) {                    /* $A1E7 */
            d->orderPos = 0;
            ntgs_reset_volumes(d);
            d->tempo = d->tempoHdr;
        } else {
            ntgs_end_of_song(d);
            return 0;
        }
    }
    d->orderPos++;
    d->row = 0;
    return 1;
}

/* Traite une ligne ($A26B). `audible` = 0 pendant une simulation (length). */
static void ntgs_process_row(RewampDecoder* d, int audible) {
    const int pat = (d->orderPos >= 1 && d->orderPos <= d->songLen) ? d->order[d->orderPos - 1] : -1;
    for (int c = 0; c < d->nch; c++) {
        const uint8_t* cell = ntgs_cell(d, pat, d->row, c);
        if (!cell) continue;
        NtgsSlot* s = &d->slot[ntgs_slot_of_chan(d, c)];
        const int note = cell[0] & 0x7f;
        const int cmd = cell[1] & 0xc0;
        const int nIns = s->isA ? d->nRam : d->nWave;
        NtgsInstr* tab = s->isA ? d->instrA : d->instrB;
        if (note == 0) {
            if (cmd == 0x40) {                         /* volume seul */
                const int ins = s->lastInstr;
                if (ins >= 1 && ins <= nIns) {
                    tab[ins - 1].volCur = cell[2];
                    if (audible) ntgs_set_volume(d, s, cell[2]);
                }
            } else if (cmd == 0x80) {
                d->tempo = cell[2];
            }
            continue;
        }
        if (note == 0x54) { d->row = 63; continue; }   /* cassure de motif */
        if (note == 0x55) {                            /* arrêt de voix */
            s->osc[0].ctl = 3; s->osc[1].ctl = 3; s->st.active = 0;   /* $A47E */
            if (!s->isA && audible) { NTGS_DOCW(d, s, 0, 0xa0, 3); NTGS_DOCW(d, s, 1, 0xa0, 3); }
            continue;
        }
        const uint16_t f = kFreq[note];
        int ins = cell[1] & 0x3f;
        if (ins == 0) { ins = s->lastInstr; if (ins == 0) continue; }
        s->lastInstr = ins;
        if (cmd == 0x80) d->tempo = cell[2];
        if (ins > nIns) continue;                      /* hors bornes: rien */
        NtgsInstr* in = &tab[ins - 1];
        in->volCur = (cmd == 0x40) ? cell[2] : in->volDef;
        s->note = note;
        if (!audible) continue;
        if (s->isA) ntgs_trigger_a(d, s, in, f);
        else        ntgs_trigger_b(d, s, in, f);
        vgm_last_note[s->voice]  = (unsigned int)(ntgs_note_hz(f) + 0.5);
        vgm_last_vol[s->voice]   = in->volCur ? in->volCur : 1;
        vgm_last_instr[s->voice] = (unsigned char)((s->isA ? 0 : d->nRam) + ins > 255
                                                   ? 255 : (s->isA ? 0 : d->nRam) + ins);
    }
    d->rowsPlayed++;
    if (++d->row >= 64) {
        if (!ntgs_next_pattern(d)) return;
    }
    if (d->tempo == 0) ntgs_end_of_song(d);            /* timer arrêté = fin */
}

static void ntgs_restart(RewampDecoder* d) {
    for (int sl = 0; sl < NTGS_MAX_SLOTS; sl++) {
        NtgsSlot* s = &d->slot[sl];
        memset(s->osc, 0, sizeof(s->osc));
        s->osc[0].ctl = s->osc[1].ctl = 1;
        memset(&s->st, 0, sizeof(s->st));
        s->lastInstr = 0;
        s->note = 0;
    }
    d->tempo = d->tempoHdr;
    ntgs_reset_volumes(d);
    d->orderPos = 0;
    d->ended = 0;
    d->rowsPlayed = 0;
    d->cur = 0;
    d->nextRow = 0.0;
    if (d->songLen <= 0 || d->tempo == 0) { d->ended = 1; return; }
    ntgs_next_pattern(d);
}

/* Durée d'un passage: simule les lignes sans mixer. */
static void ntgs_compute_length(RewampDecoder* d) {
    ntgs_restart(d);
    double t = 0.0;
    uint64_t rows = 0;
    const uint64_t maxRows = 256u * 64u + 16u;
    while (!d->ended && rows < maxRows) {
        const int before = d->orderPos;
        ntgs_process_row(d, 0);
        rows++;
        if (d->ended) break;
        t += ntgs_row_len(d->tempo ? d->tempo : 1);
        if (d->loopFlag && d->orderPos == 1 && before == d->songLen && d->row == 0) break;
    }
    d->lengthFrames = (uint64_t)(t + 0.5);
    d->passRows = rows;
    ntgs_restart(d);
}

/* ── Interface du greffon ───────────────────────────────────────────────── */

static void ntgs_read_instr(NtgsInstr* in, const uint8_t* r) {
    int k = 0;
    for (int i = 0; i < 16; i++) {
        char c = (char)(r[i] & 0x7f);
        in->name[k++] = (c >= 32 && c < 127) ? c : ' ';
    }
    in->name[k] = 0;
    while (k > 0 && in->name[k - 1] == ' ') in->name[--k] = 0;
    in->sizePages = rd16(r + 0x11);
    in->srcPage   = rd16(r + 0x13);
    in->volDef = in->volCur = r[0x15];
    in->docPage = r[0x17];
    in->wsr0 = r[0x19]; in->ctl0 = (uint8_t)(r[0x1a] & 0x0f);
    in->wsr1 = r[0x1b]; in->ctl1 = (uint8_t)(r[0x1c] & 0x0f);
    if (in->ctl1 == 0x0b) in->ctl1 = 0x07;   /* $53F3 */
    in->chan0 = r[0x1d]; in->chan1 = r[0x1e];
    in->loop = rd16(r + 0x19);
    in->loopStart = rd16(r + 0x1b);
}

static void ntgs_build_docram(RewampDecoder* d) {
    memset(d->docRam, 0, sizeof(d->docRam));
    for (int i = 0; i < d->nWave; i++) {
        const NtgsInstr* in = &d->instrB[i];
        uint64_t src;
        if (in->srcPage & 0x8000) {       /* données partagées avec le bloc A */
            const uint32_t p = (in->srcPage + (uint32_t)d->ramPages) & 0xffff;
            src = (uint64_t)d->ramOfs + (uint64_t)p * 256u;
        } else {
            src = (uint64_t)d->waveOfs + (uint64_t)in->srcPage * 256u;
        }
        uint32_t dst = (uint32_t)in->docPage * 256u;
        uint64_t n = (uint64_t)in->sizePages * 256u;
        if (dst + n > sizeof(d->docRam)) n = sizeof(d->docRam) - dst;
        for (uint64_t k = 0; k < n; k++) {
            const uint64_t o = src + k;
            d->docRam[dst + k] = o < d->fileSize ? d->file[o] : 0;
        }
    }
}

static void ntgs_close(RewampDecoder* d) {
    if (!d) return;
    free(d->file);
    free(d->instrA);
    free(d->instrB);
    free(d);
}

static RewampDecoder* ntgs_open(const char* path, RewampAudioFormat* outFormat) {
    if (!path) return NULL;
    char cleanPath[4096];
    strncpy(cleanPath, path, sizeof(cleanPath) - 1);
    cleanPath[sizeof(cleanPath) - 1] = '\0';
    char* q = strrchr(cleanPath, '?');
    if (q && strncmp(q, "?subsong=", 9) == 0) *q = '\0';   /* une seule chanson */

    FILE* fp = fopen(cleanPath, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len < 0x25e || (unsigned long)len > NTGS_MAX_FILE) { fclose(fp); return NULL; }
    RewampDecoder* d = (RewampDecoder*)calloc(1, sizeof(*d));
    if (d) d->speed = 1.0;
    if (!d) { fclose(fp); return NULL; }
    d->file = (uint8_t*)malloc((size_t)len);
    if (!d->file || fread(d->file, 1, (size_t)len, fp) != (size_t)len) {
        fclose(fp); ntgs_close(d); return NULL;
    }
    fclose(fp);
    d->fileSize = (size_t)len;
    const uint8_t* h = d->file;
    if (memcmp(h + 2, "FTA MODULEFILE", 14) != 0) { ntgs_close(d); return NULL; }

    d->tempoHdr  = h[0x10];                 /* octet bas seulement */
    d->loopFlag  = rd16(h + 0x12) != 0;
    d->songLen   = rd16(h + 0x16);
    d->npat      = rd16(h + 0x18);
    d->nWave     = rd16(h + 0x1a);
    d->nRam      = rd16(h + 0x1c);
    d->streamCfg = rd16(h + 0x1e) != 0;
    d->wavePages = rd16(h + 0x20);
    d->ramPages  = rd16(h + 0x22);
    d->nA        = rd16(h + 0x24);
    d->nch       = rd16(h + 0x28);
    if (d->songLen > 256) d->songLen = 256;
    if (d->nA > 7 || d->nch < d->nA || d->nch - d->nA > 15 - 2 * d->nA ||
        d->nWave > 63 || d->nRam > 63 || d->npat > 1024) {
        ntgs_close(d); return NULL;
    }
    for (int i = 0; i < 256; i++) d->order[i] = rd16(h + 0x5e + 2 * i);
    for (int c = 0; c < d->nch; c++) d->voiceWord[c] = rd16(h + 0x40 + 2 * c);
    d->patOfs = 0x25e;
    size_t o = d->patOfs + (size_t)d->npat * 64u * (size_t)d->nch * 3u;
    const size_t instrEnd = o + 32u * (size_t)(d->nRam + d->nWave);
    if (instrEnd > d->fileSize) { ntgs_close(d); return NULL; }
    d->instrA = (NtgsInstr*)calloc((size_t)d->nRam + 1, sizeof(NtgsInstr));
    d->instrB = (NtgsInstr*)calloc((size_t)d->nWave + 1, sizeof(NtgsInstr));
    if (!d->instrA || !d->instrB) { ntgs_close(d); return NULL; }
    for (int i = 0; i < d->nRam; i++)  ntgs_read_instr(&d->instrA[i], h + o + 32u * (size_t)i);
    o += 32u * (size_t)d->nRam;
    for (int i = 0; i < d->nWave; i++) ntgs_read_instr(&d->instrB[i], h + o + 32u * (size_t)i);
    o += 32u * (size_t)d->nWave;
    o = (o + 255u) & ~(size_t)255u;          /* alignement sur une page */
    d->ramOfs  = o;
    d->waveOfs = o + (size_t)d->ramPages * 256u;
    ntgs_build_docram(d);

    /* Voix: une par canal du fichier, A puis B. */
    d->nVoices = d->nch;
    for (int c = 0; c < d->nch; c++) {
        NtgsSlot* s = &d->slot[ntgs_slot_of_chan(d, c)];
        s->used = 1;
        s->isA = c < d->nA;
        s->voice = c;
        s->word = d->voiceWord[c];
    }

    ntgs_compute_length(d);

    rewamp_channel_data_reset(d->nVoices);
    rewamp_channel_data_set_ring_write_size(NTGS_RING_SIZE);
    rewamp_voices_meta_reset();
    if (d->nA > 0) rewamp_voices_add_chip("DOC (RAM)", 0, d->nA);
    if (d->nch > d->nA) rewamp_voices_add_chip("Ensoniq DOC", d->nA, d->nch - d->nA);
    for (int c = 0; c < d->nch; c++) {
        char nm[8];
        if (c < d->nA) snprintf(nm, sizeof(nm), "A%d", c + 1);
        else snprintf(nm, sizeof(nm), "B%d", c - d->nA + 1);
        rewamp_voice_set_name(c, nm);
    }
    for (int i = 0; i < d->nRam && i + 1 < MODIZ_MAX_INSTR; i++)
        if (d->instrA[i].name[0]) rewamp_instrument_set_name(i + 1, d->instrA[i].name);
    for (int i = 0; i < d->nWave && d->nRam + i + 1 < MODIZ_MAX_INSTR; i++)
        if (d->instrB[i].name[0]) rewamp_instrument_set_name(d->nRam + i + 1, d->instrB[i].name);

    {
        const double secs = (double)d->lengthFrames / NTGS_RATE;
        rewamp_track_message_append("Format: NoiseTracker GS 1.x (FTA MODULEFILE, Apple IIgs)\n");
        rewamp_track_message_append("Voices: %d (%d streamed RAM, %d Ensoniq DOC)\n",
                                    d->nch, d->nA, d->nch - d->nA);
        rewamp_track_message_append("Tempo: %d · Song length: %d · Patterns: %d · Rows per pass: %llu\n",
                                    d->tempoHdr, d->songLen, d->npat,
                                    (unsigned long long)d->passRows);
        rewamp_track_message_append("Duration: %u:%02u%s\n", (unsigned)(secs / 60),
                                    (unsigned)secs % 60, d->loopFlag ? " (loops)" : "");
        if (d->nRam) {
            rewamp_track_message_append("RAM instruments:\n");
            for (int i = 0; i < d->nRam; i++)
                rewamp_track_message_append("  %02d %s\n", i + 1, d->instrA[i].name);
        }
        if (d->nWave) {
            rewamp_track_message_append("DOC instruments:\n");
            for (int i = 0; i < d->nWave; i++)
                rewamp_track_message_append("  %02d %s\n", i + 1, d->instrB[i].name);
        }
    }

    if (outFormat) {
        outFormat->channels = 2;
        outFormat->sampleRate = NTGS_RATE;
    }
    return d;
}

/* Avance le temps jusqu'à `target` (frames), en mixant dans `out` si non NULL. */
static uint64_t ntgs_render(RewampDecoder* d, float* out, uint64_t frames, int capture) {
    uint64_t done = 0;
    while (done < frames && !d->ended) {
        if ((double)d->cur >= d->nextRow) {
            ntgs_process_row(d, 1);
            if (d->ended) break;
            d->nextRow += ntgs_row_len(d->tempo) / d->speed;
            continue;
        }
        uint64_t n = frames - done;
        const double untilRow = d->nextRow - (double)d->cur;
        uint64_t toRow = (uint64_t)ceil(untilRow);
        if (toRow < 1) toRow = 1;
        if (n > toRow) n = toRow;
        if (n > NTGS_CHUNK) n = NTGS_CHUNK;
        ntgs_mix(d, out ? out + done * 2 : NULL, (int)n, capture);
        d->cur += n;
        done += n;
    }
    return done;
}

static void ntgs_update_vu(RewampDecoder* d) {
    for (int sl = 0; sl < NTGS_MAX_SLOTS; sl++) {
        NtgsSlot* s = &d->slot[sl];
        if (!s->used) continue;
        const int on = s->isA ? s->st.active : !((s->osc[0].ctl & 1) && (s->osc[1].ctl & 1));
        if (!on) { vgm_last_note[s->voice] = 0; vgm_last_vol[s->voice] = 0; }
    }
}

static uint64_t ntgs_read(RewampDecoder* d, float* out, uint64_t frameCount) {
    if (!d || !out || frameCount == 0 || d->ended) return 0;
    const uint64_t n = ntgs_render(d, out, frameCount, 1);
    ntgs_update_vu(d);
    return n;
}

static void ntgs_seek(RewampDecoder* d, uint64_t frameIndex) {
    if (!d) return;
    ntgs_restart(d);
    /* Rejoue sans produire de son ni toucher aux anneaux d'oscilloscope. Cible
     * en temps du MORCEAU ⇒ ÷ vitesse en sortie. */
    ntgs_render(d, NULL, (uint64_t)llround((double)frameIndex / d->speed), 0);
    ntgs_update_vu(d);
}

static uint64_t ntgs_length(RewampDecoder* d) {
    return d ? d->lengthFrames : 0;
}

/* Vitesse NATIVE: les lignes du motif se rapprochent, le DOC garde son
 * horloge — tempo changé, hauteur conservée. */
static int ntgs_set_speed(RewampDecoder* d, double speed) {
    if (!d || speed < 0.2 || speed > 3.0) return 0;
    d->speed = speed;
    return 1;
}

static const RewampPluginVTable kNtgsVTable = {
    "NoiseTracker GS",
    ntgs_probe,
    ntgs_open,
    ntgs_read,
    ntgs_seek,
    ntgs_length,
    ntgs_close,
    NULL, 0, NULL, NULL,          /* configure_loop … param_changed */
    NULL, NULL, NULL, NULL, NULL, /* pattern view */
    NULL, 0,                      /* probe_path, noPrefixProbe */
    ntgs_set_speed,
};

const RewampPluginVTable* rewamp_ntgs_plugin(void) { return &kNtgsVTable; }

#endif /* REWAMP_WITH_NTGS */
