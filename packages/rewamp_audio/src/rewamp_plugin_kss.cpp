// rewamp_plugin_kss.cpp — libkss decoder plugin (MSX chiptunes).
//
// Plays the MSX music formats libkss covers, auto-detected by content:
//   .kss  — native KSS/KSSX ROM dumps (also claimed by libgme; we score above
//           it so the per-chip voice grouping below wins)
//   .mgs  — MGSDRV                .bgm — Kinrou5
//   .mpk  — MuSICA MPK 103/106    .mbm — MoonBlaster
//   .opx  — OPX                   .mus — MSX music (MGS/… by content)
//
// libkss mixes up to four MSX sound chips; the exact set is stamped in the KSS
// header flags. The Modizer per-voice scope/notes/mute capture lives inside the
// vendored cores (kssplay.c + the emu2149/emu2212/emu2413/emu8950/emu76489
// cores write m_voice_buff[] via m_voicesForceOfs and honor generic_mute_mask —
// grep YOYOFR). This plugin wires playback + the chip grouping metadata that the
// cores' fixed voice-offset order expects:
//   [Y8950 15][YM2413 14][SN76489 4 | AY-PSG 3][Konami SCC 5]
// (OPL/OPLL only when the header advertises them; PSG + SCC are always present,
// matching kssplay.c's calc_stereo offset advances).

#ifdef REWAMP_WITH_KSS

#include "rewamp_plugin.h"
#include "rewamp_channel_data.h"   // per-voice scope + chip grouping

extern "C" {
#include "kssplay.h"               // KSSPLAY API + full struct (opll_stereo, kss…)
#include "kss.h"                   // KSS struct, KSS_load_file, KSS_check_type
#include "mus2kss.h"               // FAC Soundtracker .MUS → image KSS
#include "rewamp_loaded_files.h"   // le kit .SM1/.SM2 est un fichier COMPAGNON
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <vector>
#include <algorithm>
#include <strings.h>

// kssplay.c's YOYOFR post-loop reads this global to know how many voice buffers
// to filter — the plugin owns it (like openmpt/sid/v2m/hvl).
extern "C" int m_genNumVoicesChannels;

#define KSS_CHANNELS 2
#define KSS_RATE     44100
// kssplay.c voice ring mask: SOUND_BUFFER_SIZE_SAMPLE*4*2 = 512*4*2.
#define KSS_RING_SAMPLES (512 * 4 * 2)

struct RewampDecoder {
    KSSPLAY*  play;
    KSS*      kss;
    int       track;      // absolute KSS track number
    int       rate;
    int16_t*  pcm;        // scratch interleaved int16 stereo
    uint64_t  pcmFrames;
    uint64_t  lengthFrames;
    double    speed;      // vitesse native (0 = 1×)
    double    baseVsync;  // fréquence VSYNC du fichier, relevée à la 1re vitesse
};

static const char* const kKssExts[] = {
    "kss", "mgs", "bgm", "mpk", "mbm", "opx", "mus", NULL
};

// Un `.MUS` de FAC Soundtracker est un fichier BSAVE MSX: `FE`, adresse de
// début, adresse de fin, adresse d'exécution, puis 0x4000 octets de données
// chargés en #8000. La signature est donc `FE 00 80 FF BF` — début #8000, fin
// #BFFF, soit exactement les 16384 octets que le format décrit.
//
// ⚠️ Tester le seul `FE` (ce que fait `mus2kss_is_mus_file`) ne suffirait PAS
// pour une SONDE: c'est le marqueur BSAVE, que portent aussi d'autres fichiers
// MSX. Les adresses, elles, sont propres à ce format.
static int kss_is_fac_mus(const char* ext, const uint8_t* hdr, size_t n) {
    if (!ext || strcmp(ext, "mus") != 0) return 0;
    if (!hdr || n < 5) return 0;
    return hdr[0] == 0xFE && hdr[1] == 0x00 && hdr[2] == 0x80 &&
           hdr[3] == 0xFF && hdr[4] == 0xBF;
}

static int kss_probe(const char* ext, const uint8_t* hdr, size_t n) {
    if (!ext || !rewamp_ext_in_list(ext, kKssExts)) return 0;
    // FAC Soundtracker: libkss ne connaît PAS ce format — `KSS_check_type` le
    // rendrait UNKNOWN et ce greffon ne serait jamais choisi, alors que
    // libsidplayfp revendique `.mus` à 60 (format MUS du C64) et l'emporterait.
    // C'est `kss_load_any` qui l'enveloppe ensuite avec le replayer FAC.
    if (kss_is_fac_mus(ext, hdr, n)) return 95;
    // Confirm the bytes actually decode to a libkss format (KSS_check_type reads
    // magic for MGS/MPK/OPX/BGM/KSS and the extension for MBM). This declines
    // e.g. Doom-style .mus files so we don't hijack them.
    char fakename[32];
    snprintf(fakename, sizeof(fakename), "probe.%s", ext);
    int type = KSS_check_type((uint8_t*)hdr, (uint32_t)n, fakename);
    if (type == KSS_TYPE_UNKNOWN) return 0;
    // .kss is shared with libgme (header score 100); beat it for the voice model.
    return (strcmp(ext, "kss") == 0) ? 101 : 90;
}

// Register the chip groups in the exact order kssplay.c advances m_voicesForceOfs
// so each chip's scope writes land in the matching voice slots. Returns total.
static int kss_setup_voices(const KSS* kss) {
    rewamp_voices_meta_reset();
    char nm[24];
    int total = 0;
    #define ADD_CHIP(label, cnt)                                            \
        do {                                                                \
            rewamp_voices_add_chip(label, total, (cnt));                    \
            for (int i = 0; i < (cnt); i++) {                               \
                snprintf(nm, sizeof(nm), label " %d", i + 1);              \
                rewamp_voice_set_name(total + i, nm);                       \
            }                                                               \
            total += (cnt);                                                 \
        } while (0)

    if (kss->msx_audio) ADD_CHIP("Y8950", 15);   // MSX-Audio (OPL, ch14 = ADPCM)
    if (kss->fmpac)     ADD_CHIP("YM2413", 14);  // FMPAC (OPLL)
    if (kss->sn76489)   ADD_CHIP("SN76489", 4);  // SG/GG/SMS PSG
    else                ADD_CHIP("PSG", 3);      // AY-3-8910 (YM2149)
    ADD_CHIP("SCC", 5);                          // Konami SCC (always mixed)

    #undef ADD_CHIP
    return total;
}


// Lit un fichier entier. Rend NULL (sans bruit) s'il n'existe pas: le kit de
// percussions est OPTIONNEL et son absence est un cas normal, pas une erreur.
static uint8_t* kss_read_all(const char* path, size_t* outSize) {
    *outSize = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return NULL; }
    uint8_t* buf = (uint8_t*)malloc((size_t)size);
    if (!buf) { fclose(f); return NULL; }
    const size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buf); return NULL; }
    *outSize = got;
    return buf;
}

// ── FAC Soundtracker (.MUS) ──────────────────────────────────────────────────
//
// Un `.mus` de FAC Soundtracker n'est PAS un format que libkss connaît:
// `KSS_check_type` détecte MGS, MPK, KSS/KSSX, OPX et BGM par leur CONTENU, MBM
// par son extension, et rien d'autre — un `.mus` FAC y tombe donc sur
// `KSS_TYPE_UNKNOWN`, et `KSS_bin2kss` rend NULL. C'est pour ça qu'il ne jouait
// pas, alors que l'extension était revendiquée depuis toujours.
//
// `mus2kss` (script d'origine par NYYRIKKI, porté en C par le projet modizer)
// enveloppe les données avec FST2.BIN — le replayer FAC de 1990/1991 — dans une
// image KSS que la VM Z80 de libkss exécute. C'est le patron NATIF de libkss:
// `mgs2kss`, `mbm2kss`, `opx2kss`… embarquent tous le driver MSX d'origine.
// L'intérêt est décisif: c'est le replayer AUTHENTIQUE qui joue, donc aucune
// sémantique à deviner — la spécification publique du format documente les
// offsets mais PAS les enveloppes, les effets ni le tempo.
//
// ⚠️ Ordre d'essai: le contenu D'ABORD, la conversion ENSUITE. `.mus` est une
// extension partagée (MuSICA et consorts), et `mus2kss_is_mus_file` ne teste
// que l'octet BSAVE `0xFE`, que ces formats portent aussi. Tenter la conversion
// en premier produirait du bruit sur un fichier d'une autre famille; l'inverse
// ne peut rien casser, la détection de libkss étant fondée sur le contenu.
// MoonBlaster (.MBM) — son kit d'échantillons MSX-AUDIO est un fichier
// COMPAGNON, le `.MBK`, et libkss ne le trouve que si on lui dit où chercher.
//
// `KSS_mbm2kss` charge le kit par `load_mbk()`, qui essaie les quatre chemins
// que pose `KSS_autoload_mbk` — et personne ne l'appelait: les chemins
// restaient VIDES, donc AUCUN .MBM ne chargeait son kit (panneau ⓘ: « MBK is
// not Found »), et tout ce qui passe par les échantillons ADPCM manquait.
//
// Le nom du kit est dans l'en-tête, 8 caractères à l'offset 0x140 — 0x141
// pour un fichier sauvé en mode ÉDITION (`MoonBlaster (edit mode)` chez
// modland), qui porte un octet 0xFF de plus en tête et décale tout
// l'en-tête d'un octet (vérifié: ses 320 premiers octets sont ceux de la
// version normale, décalés). On résout nous-mêmes, SANS casse (un rip porte
// « SP_ACE.MBK » comme « sp_ace.mbk »): d'abord le kit du même nom que le
// morceau, puis celui que l'en-tête nomme. Le chemin trouvé passe à libkss
// comme quatrième candidat (`dummy_file`), et au panneau ⓘ.
static bool kss_find_ci(const char* dir, const char* name, char* out, size_t cap) {
    DIR* d = opendir(dir[0] ? dir : ".");
    if (!d) return false;
    bool found = false;
    while (struct dirent* e = readdir(d)) {
        if (strcasecmp(e->d_name, name) == 0) {
            snprintf(out, cap, "%s", e->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static void kss_prepare_mbk(const char* cleanPath) {
    const char* dot = strrchr(cleanPath, '.');
    if (!dot || strcasecmp(dot, ".mbm") != 0) return;

    const char* slash = strrchr(cleanPath, '/');
#ifdef _WIN32
    const char* bslash = strrchr(cleanPath, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    char dir[4096] = ".";
    if (slash) {
        size_t n = (size_t)(slash - cleanPath);
        if (n >= sizeof(dir)) return;
        memcpy(dir, cleanPath, n);
        dir[n] = '\0';
    }

    unsigned char head[0x149] = {0};
    FILE* f = fopen(cleanPath, "rb");
    size_t got = 0;
    if (f) { got = fread(head, 1, sizeof(head), f); fclose(f); }

    // 1) Le kit du même nom que le morceau (« SONG.MBM » → « SONG.MBK »).
    char want[512];
    const char* base = slash ? slash + 1 : cleanPath;
    snprintf(want, sizeof(want), "%.*sMBK", (int)(dot - base) + 1, base);
    char kit[512] = {0};
    bool ok = kss_find_ci(dir, want, kit, sizeof(kit));

    // 2) Celui que l'en-tête nomme (0x140, ou 0x141 en mode édition).
    if (!ok && got >= sizeof(head)) {
        const size_t off = head[0] == 0xFF ? 0x141 : 0x140;
        char name[16];
        size_t i = 0;
        for (; i < 8 && head[off + i] > ' ' && head[off + i] < 0x7F; i++)
            name[i] = (char)head[off + i];
        name[i] = '\0';
        if (i > 0) {
            snprintf(want, sizeof(want), "%s.MBK", name);
            ok = kss_find_ci(dir, want, kit, sizeof(kit));
        }
    }

    // Toujours appelé pour un .MBM: les chemins de libkss sont GLOBAUX, et ceux
    // du morceau précédent feraient sinon charger son kit à lui.
    KSS_autoload_mbk(cleanPath, dir, ok ? kit : NULL);
    if (ok) {
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir, kit);
        rewamp_loaded_files_add(full);
    }
}

// MoonBlaster sauvé en mode ÉDITION (modland « MoonBlaster (edit mode) », 129
// fichiers) → mode UTILISATEUR, en mémoire. Le replayer MBR143 que libkss
// embarque ne lit que le second: un fichier édition ne produisait aucune note,
// ou jouait faux (« ACE IN SPACE LEVEL 1 »).
//
// Les MOTIFS sont compactés de la même façon dans les deux modes; seule
// l'ENVELOPPE diffère (format: msx.org « MBM file format », et le lecteur
// RoboPlay `players/src/mbm.c`, qui charge les deux modes). Offsets relatifs à
// l'en-tête, c'est-à-dire APRÈS l'octet 0xFF de tête du mode édition:
//   0x000  longueur du morceau `sl` (+ en-tête, identique dans les deux modes)
//   0x178  positions — UTILISATEUR: sl+1 octets; ÉDITION: 201 octets fixes
//   puis   pointeurs u16 LE vers les motifs, base = début de l'en-tête —
//          UTILISATEUR: un par motif UTILISÉ (`hp` = plus haut numéro de la
//          table des positions); ÉDITION: tous les motifs (76), dès 0x241
//   puis   les motifs; l'UTILISATEUR finit par un 0x00.
// Conversion: en-tête + positions tronquées à sl+1, puis les `hp` premiers
// motifs recopiés à la suite avec une table de pointeurs refaite. La fin d'un
// motif est le DÉBUT du motif suivant en mémoire (pointeurs triés), pas
// celui du numéro suivant. Vérifié: depuis « level 1 BIG BOSS » édition, la
// sortie égale la version utilisateur de modland octet pour octet, au nom du
// kit près (les deux fichiers de modland n'ont pas le même).
static uint8_t* kss_mbm_edit_to_user(const uint8_t* raw, size_t rawSize,
                                     size_t* outSize) {
    if (rawSize < 1 + 0x241 + 2 || raw[0] != 0xFF) return NULL;
    const uint8_t* e = raw + 1;
    const size_t n = rawSize - 1;
    auto u16 = [&](size_t o) -> size_t { return (size_t)e[o] | ((size_t)e[o + 1] << 8); };

    const size_t sl = e[0];
    if (0x178 + sl + 1 > 0x241) return NULL;          // dépasserait les 201 cases
    size_t hp = 0;
    for (size_t i = 0; i <= sl; i++) if (e[0x178 + i] > hp) hp = e[0x178 + i];
    if (hp == 0) return NULL;

    const size_t p0 = u16(0x241);                     // 1er motif = fin de la table
    if (p0 <= 0x241 || p0 > n || ((p0 - 0x241) & 1)) return NULL;
    const size_t np = (p0 - 0x241) / 2;               // 76 dans les fichiers vus
    if (hp > np) return NULL;
    std::vector<size_t> ptr(np), sorted;
    for (size_t i = 0; i < np; i++) {
        ptr[i] = u16(0x241 + 2 * i);
        if (ptr[i] < p0 || ptr[i] > n) return NULL;
    }
    sorted = ptr;
    std::sort(sorted.begin(), sorted.end());

    const size_t head = 0x178 + sl + 1;
    std::vector<uint8_t> out(e, e + head);
    const size_t base = head + 2 * hp;
    std::vector<uint8_t> data;
    for (size_t i = 0; i < hp; i++) {
        const size_t start = ptr[i];
        auto nx = std::upper_bound(sorted.begin(), sorted.end(), start);
        const size_t end = nx != sorted.end() ? *nx : n;
        const size_t at = base + data.size();
        if (at > 0xFFFF) return NULL;
        out.push_back((uint8_t)(at & 0xFF));
        out.push_back((uint8_t)(at >> 8));
        data.insert(data.end(), e + start, e + end);
    }
    out.insert(out.end(), data.begin(), data.end());
    out.push_back(0x00);

    uint8_t* buf = (uint8_t*)malloc(out.size());
    if (!buf) return NULL;
    memcpy(buf, out.data(), out.size());
    *outSize = out.size();
    return buf;
}

// Un .MBM en mode édition, converti puis remis à libkss par la même porte que
// KSS_load_file (KSS_bin2kss, l'extension du chemin fait le type). NULL pour
// tout autre fichier.
static KSS* kss_load_mbm_edit(const char* cleanPath) {
    const char* dot = strrchr(cleanPath, '.');
    if (!dot || strcasecmp(dot, ".mbm") != 0) return NULL;
    FILE* f = fopen(cleanPath, "rb");
    if (!f) return NULL;
    int first = fgetc(f);
    if (first != 0xFF) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 0x10000) { fclose(f); return NULL; }
    std::vector<uint8_t> raw((size_t)size);
    size_t got = fread(raw.data(), 1, raw.size(), f);
    fclose(f);
    if (got != raw.size()) return NULL;
    size_t userSize = 0;
    uint8_t* user = kss_mbm_edit_to_user(raw.data(), raw.size(), &userSize);
    if (!user) return NULL;
    KSS* kss = KSS_bin2kss(user, (uint32_t)userSize, cleanPath);
    free(user);
    return kss;
}

static KSS* kss_load_any(const char* cleanPath) {
    kss_prepare_mbk(cleanPath);
    if (KSS* edit = kss_load_mbm_edit(cleanPath)) return edit;
    // 1) Ce que libkss reconnaît lui-même, y compris sous l'extension .mus.
    if (KSS* direct = KSS_load_file((char*)cleanPath)) return direct;

    // 2) Sinon, la conversion FAC.
    FILE* f = fopen(cleanPath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return NULL; }
    uint8_t* mus = (uint8_t*)malloc((size_t)size);
    if (!mus) { fclose(f); return NULL; }
    size_t got = fread(mus, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(mus); return NULL; }

    if (!mus2kss_is_mus_file(mus, got)) { free(mus); return NULL; }

    // Le kit de PERCUSSIONS est un fichier VOISIN: son nom vit à l'offset
    // `taille - 124` du .mus, et le `.SM2` se déduit du `.SM1`. Sans lui la
    // conversion réussit quand même — en mode MSX-Audio, sans percussions.
    uint8_t *sm1 = NULL, *sm2 = NULL;
    size_t   sm1Size = 0, sm2Size = 0;
    char     drumkit[16];
    if (mus2kss_get_drumkit_name(mus, got, drumkit, sizeof(drumkit)) > 0) {
        char base[4096];
        strncpy(base, cleanPath, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        char* slash = strrchr(base, '/');
        char* dirEnd = slash ? slash + 1 : base;
        // Les deux casses: un rip peut porter « KIT.SM1 » comme « kit.sm1 ».
        static const char* const kExts[2][2] = { { ".SM1", ".SM2" },
                                                 { ".sm1", ".sm2" } };
        for (int c = 0; c < 2 && !sm1; c++) {
            snprintf(dirEnd, sizeof(base) - (size_t)(dirEnd - base), "%s%s",
                     drumkit, kExts[c][0]);
            sm1 = kss_read_all(base, &sm1Size);
            if (!sm1) continue;
            rewamp_loaded_files_add(base);
            snprintf(dirEnd, sizeof(base) - (size_t)(dirEnd - base), "%s%s",
                     drumkit, kExts[c][1]);
            sm2 = kss_read_all(base, &sm2Size);
            if (sm2) rewamp_loaded_files_add(base);
        }
    }

    const size_t cap = (sm1 && sm2) ? mus2kss_get_output_size_with_drums()
                                    : mus2kss_get_output_size();
    uint8_t* kssBuf = (uint8_t*)malloc(cap);
    KSS* out = NULL;
    if (kssBuf) {
        size_t kssSize = 0;
        const int rc = (sm1 && sm2)
            ? mus2kss_convert_with_drums(mus, got, sm1, sm1Size, sm2, sm2Size,
                                         kssBuf, &kssSize)
            : mus2kss_convert(mus, got, kssBuf, &kssSize,
                              MUS2KSS_MODE_MSXAUDIO);
        if (rc == 0 && kssSize > 0)
            out = KSS_bin2kss(kssBuf, (uint32_t)kssSize, "fac.kss");
        free(kssBuf);
    }
    free(sm1);
    free(sm2);
    free(mus);
    return out;
}


// Les titres MSX (MGS, OPX, BGM…) sont du Shift-JIS: « Lost.OPX » (modland,
// My Neighbor Totoro) porte « となりのトトロ まいご » que le panneau ⓘ
// affichait en mojibake. Un titre deja en UTF-8 valide passe tel quel; le
// reste est converti (CP932 par iconv sur Apple, '?' par glyphe ailleurs).
// Les blancs de fin sont retires: opx2kss recopie le champ de 53 octets
// bourre d'espaces.
static const char* kss_text_utf8(const void* raw, char* buf, size_t cap) {
    rewamp_text_to_utf8((const char*)raw, buf, cap);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == '\t')) buf[--n] = '\0';
    return buf;
}

static RewampDecoder* kss_open(const char* path, RewampAudioFormat* outFormat) {
    if (!path) return NULL;

    // Strip ?subsong=N (rewamp convention, 0-based); keep the clean path for the
    // format converters that key off the filename extension (MBM).
    char clean[4096];
    int subsong = 0;
    strncpy(clean, path, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    char* q = strrchr(clean, '?');
    if (q && strncmp(q, "?subsong=", 9) == 0) { subsong = atoi(q + 9); *q = '\0'; }

    KSS* kss = kss_load_any(clean);
    if (!kss) return NULL;

    // ?subsong=N is the ABSOLUTE KSS song number (KSSPLAY_reset's argument), the
    // same value the native probe advertises (base+i) and the server/m3u raw
    // numbers carry. A bare open (N=0) or an out-of-range value clamps into the
    // file's [trk_min, trk_max] range → the first song. (Adding trk_min here would
    // double-count the base for KSSX files whose first song is >0.)
    int track = subsong;
    if (track < (int)kss->trk_min) track = (int)kss->trk_min;
    if (track > (int)kss->trk_max) track = (int)kss->trk_max;

    KSSPLAY* play = KSSPLAY_new(KSS_RATE, KSS_CHANNELS, 16);
    if (!play) { KSS_delete(kss); return NULL; }
    KSSPLAY_set_data(play, kss);

    // Per-voice scope + chip grouping. Allocate the voice buffers BEFORE the
    // first render — kssplay.c/emu cores write m_voice_buff[*] unguarded.
    int voices = kss_setup_voices(kss);
    m_genNumVoicesChannels = voices;
    rewamp_channel_data_reset(voices);
    rewamp_channel_data_set_ring_write_size(KSS_RING_SAMPLES);
    // The cores wrap m_voice_current_ptr mod the ring (not monotonic) → circular
    // read so the scope shows a full window instead of blanking on each wrap.
    rewamp_channel_data_set_ring_circular(1);

    KSSPLAY_reset(play, (uint32_t)track, 0);

    // Mixer setup (mirrors Modizer): higher device quality, stereo spread.
    KSSPLAY_set_device_quality(play, EDSC_PSG, 1);
    KSSPLAY_set_device_quality(play, EDSC_SCC, 1);
    KSSPLAY_set_device_quality(play, EDSC_OPLL, 1);
    KSSPLAY_set_master_volume(play, 64);
    KSSPLAY_set_device_pan(play, EDSC_PSG, -32);
    KSSPLAY_set_device_pan(play, EDSC_SCC,  32);
    play->opll_stereo = 1;
    for (int c = 0; c < 6; c++)
        KSSPLAY_set_channel_pan(play, EDSC_OPLL, c, (c & 1) ? 2 : 1);

    RewampDecoder* dec = (RewampDecoder*)calloc(1, sizeof(*dec));
    if (!dec) { KSSPLAY_delete(play); KSS_delete(kss); return NULL; }
    dec->play  = play;
    dec->kss   = kss;
    dec->track = track;
    dec->rate  = KSS_RATE;

    // Duration from the KSS info table (ms) when present, else host default.
    if (kss->info && track < (int)kss->info_num && kss->info[track].time_in_ms > 0)
        dec->lengthFrames = (uint64_t)kss->info[track].time_in_ms * KSS_RATE / 1000;

    // Info panel metadata.
    char text[KSS_TITLE_MAX * 3];
    if (kss->title[0]) rewamp_track_message_append("Title: %s\n", kss_text_utf8(kss->title, text, sizeof(text)));
    if (kss->info && track < (int)kss->info_num && kss->info[track].title[0])
        rewamp_track_message_append("Track: %s\n", kss_text_utf8(kss->info[track].title, text, sizeof(text)));
    int subCount = (int)kss->trk_max - (int)kss->trk_min + 1;
    if (subCount > 1) rewamp_track_message_append("Subsongs: %d\n", subCount);
    if (kss->extra && kss->extra[0])
        rewamp_track_message_append("\n%s\n", (const char*)kss->extra);

    outFormat->channels   = KSS_CHANNELS;
    outFormat->sampleRate  = (uint32_t)KSS_RATE;
    return dec;
}

static uint64_t kss_read_frames(RewampDecoder* dec, float* out, uint64_t frameCount) {
    if (!dec || !dec->play || !out || frameCount == 0) return 0;
    if (frameCount > dec->pcmFrames) {
        free(dec->pcm);
        dec->pcm = (int16_t*)malloc(frameCount * KSS_CHANNELS * sizeof(int16_t));
        dec->pcmFrames = dec->pcm ? frameCount : 0;
    }
    if (!dec->pcm) return 0;

    KSSPLAY_calc(dec->play, dec->pcm, (uint32_t)frameCount);
    const int samples = (int)(frameCount * KSS_CHANNELS);
    const float inv = 1.0f / 32768.0f;
    for (int i = 0; i < samples; i++) out[i] = dec->pcm[i] * inv;
    return frameCount;   // KSS loops; the host enforces length/fade
}

/* Vitesse NATIVE: la fréquence VSYNC qui appelle la routine de jeu, × vitesse
 * (VM_set_clock par KSSPLAY_set_speed); le Z80 et les puces gardent leur
 * horloge — tempo changé, hauteur conservée. KSSPLAY_reset garde une
 * vsync_freq non nulle: la vitesse survit au seek. */
static double kss_base_vsync(RewampDecoder* dec) {
    if (dec->baseVsync <= 0.0) dec->baseVsync = dec->play->vsync_freq;
    return dec->baseVsync;
}
static int kss_set_speed(RewampDecoder* dec, double speed) {
    if (!dec || !dec->play || speed < 0.2 || speed > 3.0) return 0;
    const double base = kss_base_vsync(dec);
    if (base <= 0.0) return 0;
    dec->speed = speed;
    dec->play->vsync_freq = base * speed;
    KSSPLAY_set_speed(dec->play, dec->play->cpu_speed);
    return 1;
}

static void kss_seek_frames(RewampDecoder* dec, uint64_t frameIndex) {
    if (!dec || !dec->play) return;
    // No random seek: reset and render-skip to the target (Modizer approach).
    // Cible en temps du MORCEAU ⇒ ÷ vitesse en trames de sortie.
    KSSPLAY_reset(dec->play, (uint32_t)dec->track, 0);
    uint64_t remaining = (uint64_t)((double)frameIndex / (dec->speed > 0.0 ? dec->speed : 1.0) + 0.5);
    while (remaining > 0) {
        uint32_t chunk = remaining > 4096 ? 4096 : (uint32_t)remaining;
        KSSPLAY_calc_silent(dec->play, chunk);
        remaining -= chunk;
    }
}

static uint64_t kss_length_frames(RewampDecoder* dec) {
    return dec ? dec->lengthFrames : 0;   // 0 → host uses the default length
}

static void kss_close(RewampDecoder* dec) {
    if (!dec) return;
    if (dec->play) KSSPLAY_delete(dec->play);
    if (dec->kss)  KSS_delete(dec->kss);
    free(dec->pcm);
    free(dec);
}

// ── Subsong probe (libkss-authoritative) ────────────────────────────────────
// libgme can't reliably count KSS tracks (a plain KSCC file carries no track
// count → GME guesses 256; KSSX indexes its first/last song differently), so the
// registry routes kss-family files here. The count is the KSS song range and the
// index i (0-based, matching kss_open's `track = trk_min + i`) maps title/duration
// through kss->info. State is kept until the next probe (rewamp_audio.c contract).
static KSS* g_probe_kss    = NULL;
static int  g_probe_trkmin = 0;
static char g_probe_title[KSS_TITLE_MAX * 3];

extern "C" int rewamp_kss_probe_subsong_count(const char* path) {
    if (g_probe_kss) { KSS_delete(g_probe_kss); g_probe_kss = NULL; }
    if (!path) return 0;
    char clean[4096];
    strncpy(clean, path, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    char* q = strrchr(clean, '?');
    if (q && strncmp(q, "?subsong=", 9) == 0) *q = '\0';

    KSS* kss = kss_load_any(clean);
    if (!kss) return 0;
    g_probe_kss    = kss;
    g_probe_trkmin = (int)kss->trk_min;
    return (int)kss->trk_max - (int)kss->trk_min + 1;
}

// The first song number (trk_min). The native-probe path adds this to its 0-based
// position so the subsong index it advertises is the ABSOLUTE KSS song number,
// matching the m3u/server raw numbers and kss_open's `track = subsong`.
extern "C" int rewamp_kss_probe_base(void) { return g_probe_trkmin; }

extern "C" const char* rewamp_kss_probe_get_title(int idx) {
    KSS* kss = g_probe_kss;
    if (!kss) return "";
    int track = g_probe_trkmin + idx;
    if (kss->info && track >= 0 && track < (int)kss->info_num && kss->info[track].title[0]) {
        return kss_text_utf8(kss->info[track].title, g_probe_title, sizeof(g_probe_title));
    }
    return "";
}

extern "C" int rewamp_kss_probe_get_duration_ms(int idx) {
    KSS* kss = g_probe_kss;
    if (!kss) return -1;
    int track = g_probe_trkmin + idx;
    if (kss->info && track >= 0 && track < (int)kss->info_num && kss->info[track].time_in_ms > 0)
        return kss->info[track].time_in_ms;
    return -1;
}

static const RewampPluginVTable kKssVTable = {
    "kss",
    kss_probe,
    kss_open,
    kss_read_frames,
    kss_seek_frames,
    kss_length_frames,
    kss_close,
    nullptr, 0, nullptr, nullptr,              /* configure_loop … param_changed */
    nullptr, nullptr, nullptr, nullptr, nullptr, /* pattern view */
    nullptr, 0,                                /* probe_path, noPrefixProbe */
    kss_set_speed,                             /* vitesse native (VSYNC) */
};

extern "C" const RewampPluginVTable* rewamp_kss_plugin(void) { return &kKssVTable; }

#endif /* REWAMP_WITH_KSS */
