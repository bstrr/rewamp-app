// SAM Coupé `.cop` joués par leur PROPRE code Z80 (z80ex + la SAA1099 de zxtune).
//
// La collection modland « Sam Coupe COP » (191 fichiers) mélange deux choses
// sous la même extension:
//   - 166 modules E-Tracker COMPILÉS (« ETracker (C) BY ESI. » dans le fichier):
//     zxtune les lit (cop_supp), ils ne passent PAS par ici;
//   - 25 programmes Z80 BRUTS qui écrivent eux-mêmes dans la SAA1099 (port
//     0x1FF = registre, 0x0FF = donnée) — « manic.cop » de František Fuka en
//     tête. zxtune réclame l'extension (55) puis échoue à l'ouverture: muets.
//
// Convention, mesurée et non supposée: l'image se charge en 0x8000 et le point
// d'entrée 0x8000 s'appelle UNE fois par trame (50 Hz, interruption trame du
// SAM). Deux familles:
//   - « B » (16 fichiers: manic, prince, splat, karate…): même lecteur à l'octet
//     près (seuls des octets de VARIABLES auto-modifiées diffèrent — le fichier
//     a été sauvé en cours de lecture). Le n° de morceau se POSE en 0x8001 avant
//     l'appel (0 = arrêt). Table des morceaux en 0x84A9: des groupes de mots
//     terminés par 0000, un groupe vide clôt la table — le compte se lit sans
//     émuler. Un morceau finit de deux façons: commande d'arrêt (le saut
//     auto-modifié de 0x8059 revient sur 0x8013) ou rebouclage de sa liste
//     d'ordres (routine en 0x81C5) — c'est la fin d'UNE passe.
//   - générique (craft/ofc*, logik/rozkaz/stovka, evropa): un seul morceau,
//     fin = silence durable, sinon durée inconnue (l'app applique sa durée
//     par défaut). craft/ofc* (« C ») ont un compteur d'init en 0x8001, remis
//     à 1 au chargement (voir Machine::load).
//
// Le son sort de Devices::SAA de zxtune — la MÊME puce que les .cop E-Tracker,
// avec ses accroches d'oscilloscope et de mute déjà en place. Compilé DANS la
// bibliothèque zxtune (mêmes defines), sous REWAMP_WITH_ZXTUNE.
#ifdef REWAMP_WITH_ZXTUNE

#include "rewamp_plugin.h"
#include "rewamp_channel_data.h"
#include "ModizerVoicesData.h"

#include <devices/saa.h>
#include <sound/chunk.h>
#include "z80ex.h"

#include <boost/make_shared.hpp>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vector>

extern "C" int g_force_loop_mode;
extern "C" int g_force_loop_count;
extern "C" int g_force_loop_native_veto;

namespace {

const int      kRate          = 44100;
const int      kFrameHz       = 50;
const int      kSamplesPerFrm = kRate / kFrameHz;          // 882, exact
const uint32_t kLoadAddr      = 0x8000;
const uint32_t kMaxImage      = 0x8000;                    // 0x8000..0xFFFF
const int      kVoices        = 6;
const uint64_t kSaaClock      = 8000000;                   // SAM Coupé: 8 MHz
const int      kMaxMeasureFrm = kFrameHz * 60 * 20;        // 20 min
const int      kSilentEndFrm  = kFrameHz * 3;              // générique: 3 s muettes
const long     kMaxStepsPerFrm = 2000000;                  // garde-fou boucle infinie

/* Lecteur « B »: octets IMMUABLES en tête (après `LD BC,#nnnn`) et la table. */
const uint8_t kSigB[] = {
    0x3e, 0xff, 0x32, 0x01, 0x80, 0x79, 0xfe, 0xff, 0x28, 0x46, 0x32, 0x02, 0x80,
    0xb7, 0x20, 0x13, 0x01, 0xff, 0x01, 0x3e, 0x1c, 0xed, 0x79, 0x05, 0x3e, 0x02,
    0xed, 0x79, 0xaf, 0x32, 0x01, 0x80, 0xc9 };
const uint8_t kSigBTable[] = { 0x21, 0xa9, 0x84 };          // LD HL,#84A9 en 0x8026
const uint16_t kBSongReq   = 0x8001;
const uint16_t kBJumpOp    = 0x805a;                         // opérande du JP de 0x8059
const uint16_t kBStopAddr  = 0x8013;
const uint16_t kBWrapPc    = 0x81c5;
const uint16_t kBTable     = 0x84a9;

const uint8_t kSigCTail[] = { 0x3d, 0xc2, 0x23, 0x81 };     // DEC A / JP NZ,#8123

const char kETracker[] = "ETracker (C) BY ESI.";

bool is_family_b(const uint8_t* img, size_t n) {
    return n >= 0x30 &&
           memcmp(img + 3, kSigB, sizeof(kSigB)) == 0 &&
           memcmp(img + 0x26, kSigBTable, sizeof(kSigBTable)) == 0;
}

bool has_etracker(const uint8_t* img, size_t n) {
    const size_t k = sizeof(kETracker) - 1;
    for (size_t i = 0; i + k <= n; i++)
        if (img[i] == 'E' && memcmp(img + i, kETracker, k) == 0) return true;
    return false;
}

int split_subsong(const char* path, char* clean, size_t cap) {
    strncpy(clean, path, cap - 1);
    clean[cap - 1] = '\0';
    int sub = 0;
    char* q = strrchr(clean, '?');
    if (q) {
        if (strncmp(q, "?subsong=", 9) == 0) sub = atoi(q + 9);
        *q = '\0';
    }
    return sub < 0 ? 0 : sub;
}

bool read_image(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    out.resize(kMaxImage);
    const size_t n = fread(out.data(), 1, kMaxImage, f);
    fclose(f);
    out.resize(n);
    return n > 0;
}

bool cop_name(const char* ext) { return ext && strcasecmp(ext, "cop") == 0; }

/* ── Machine: 64 Ko de RAM + Z80, ports SAA journalisés ─────────────────── */

struct Machine {
    uint8_t        mem[65536];
    Z80EX_CONTEXT* cpu = nullptr;
    int            reg = 0;           // registre SAA sélectionné (port 0x1FF)
    uint8_t        saa[32];           // valeurs écrites, persistantes
    uint32_t       touched = 0;       // registres écrits pendant la trame
    bool           wrapped = false;   // B: routine de rebouclage atteinte

    static Z80EX_BYTE mr(Z80EX_CONTEXT*, Z80EX_WORD a, int, void* u) {
        return static_cast<Machine*>(u)->mem[a];
    }
    static void mw(Z80EX_CONTEXT*, Z80EX_WORD a, Z80EX_BYTE v, void* u) {
        Machine* m = static_cast<Machine*>(u);
        if (a >= 2) m->mem[a] = v;        // la sentinelle HALT en 0x0000 reste intacte
    }
    static Z80EX_BYTE pr(Z80EX_CONTEXT*, Z80EX_WORD, void*) { return 0xff; }
    static void pw(Z80EX_CONTEXT*, Z80EX_WORD p, Z80EX_BYTE v, void* u) {
        Machine* m = static_cast<Machine*>(u);
        if ((p & 0xff) != 0xff) return;           // seule la SAA nous intéresse
        if (p & 0x100) { m->reg = v & 31; return; }
        m->saa[m->reg] = v;
        m->touched |= 1u << m->reg;
    }
    static Z80EX_BYTE ir(Z80EX_CONTEXT*, void*) { return 0xff; }

    Machine() {
        memset(mem, 0, sizeof(mem));
        memset(saa, 0, sizeof(saa));
        cpu = z80ex_create(mr, this, mw, this, pr, this, pw, this, ir, this);
    }
    ~Machine() { if (cpu) z80ex_destroy(cpu); }
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    void load(const std::vector<uint8_t>& img, bool familyB, int song) {
        memset(mem, 0, sizeof(mem));
        memset(saa, 0, sizeof(saa));
        reg = 0;
        touched = 0;
        memcpy(mem + kLoadAddr, img.data(), img.size());
        mem[0] = 0x76;                              // HALT: adresse de retour sentinelle
        if (familyB) mem[kBSongReq] = (uint8_t)(song + 1);
        /* Famille « C » (craft, ofc1-4): `LD A,n / DEC A / JP NZ,#8123` — n est
         * un compteur, 1 = initialiser. Le fichier le garde à la valeur du
         * moment de la sauvegarde (ofc1: 12), et craft a même ses deux premiers
         * octets écrasés par du texte (« Cr »): muet tel quel. On repart de
         * l'initialisation. */
        else if (img.size() > 6 && memcmp(&mem[kLoadAddr + 2], kSigCTail, sizeof(kSigCTail)) == 0) {
            mem[kLoadAddr]     = 0x3e;                  // LD A,
            mem[kLoadAddr + 1] = 0x01;                  //      #01
        }
        z80ex_reset(cpu);
    }

    /* Un appel du point d'entrée = une trame. Faux si le code ne rend pas la main. */
    bool frame() {
        touched = 0;
        wrapped = false;
        uint16_t sp = 0xfff0;
        mem[--sp] = 0x00;                           // RET → 0x0000
        mem[--sp] = 0x00;
        z80ex_set_reg(cpu, regSP, sp);
        z80ex_set_reg(cpu, regPC, kLoadAddr);
        long steps = 0;
        while (z80ex_get_reg(cpu, regPC) != 0) {
            if (z80ex_get_reg(cpu, regPC) == kBWrapPc) wrapped = true;
            z80ex_step(cpu);
            if (++steps > kMaxStepsPerFrm) return false;
        }
        return true;
    }

    bool audible() const {
        if (!(saa[0x1c] & 1)) return false;         // sortie coupée
        const uint8_t mix = saa[0x14] | saa[0x15];
        for (int v = 0; v < kVoices; v++)
            if (saa[v] && (mix & (1 << v))) return true;
        return false;
    }
};

int count_songs_b(const std::vector<uint8_t>& img) {
    auto word = [&](uint32_t a) -> uint32_t {
        const uint32_t o = a - kLoadAddr;
        if (a < kLoadAddr || o + 1 >= img.size()) return 0;
        return img[o] | (img[o + 1] << 8);
    };
    uint32_t hl = kBTable;
    int songs = 0;
    while (songs < 255 && word(hl) != 0) {
        songs++;
        while (hl < 0xfffe && word(hl) != 0) hl += 2;
        hl += 2;
    }
    return songs;
}

/* Fin d'une passe, en trames de 50 Hz. *stops = le morceau s'ARRÊTE (jingle)
 * plutôt que de reboucler. 0 = aucune fin trouvée. */
int measure_pass(const std::vector<uint8_t>& img, bool familyB, int song, bool* stops) {
    *stops = false;
    Machine* m = new Machine();
    m->load(img, familyB, song);
    int end = 0, silentSince = -1;
    for (int f = 0; f < kMaxMeasureFrm; f++) {
        if (!m->frame()) { end = f; *stops = true; break; }
        if (familyB) {
            const uint16_t jt = (uint16_t)(m->mem[kBJumpOp] | (m->mem[kBJumpOp + 1] << 8));
            if (f > 0 && jt == kBStopAddr) { end = f; *stops = true; break; }
            if (m->wrapped) { end = f + 1; break; }
        } else {
            if (m->audible()) silentSince = -1;
            else if (silentSince < 0) silentSince = f;
            if (silentSince >= 0 && f - silentSince >= kSilentEndFrm) {
                end = silentSince; *stops = true; break;
            }
        }
    }
    delete m;
    return end;
}

/* ── La puce: Devices::SAA de zxtune, échantillons recueillis dans une file ── */

class ChipParams : public Devices::SAA::ChipParameters {
public:
    virtual uint_t Version() const { return 1; }
    virtual uint64_t ClockFreq() const { return kSaaClock; }
    virtual uint_t SoundFreq() const { return kRate; }
    virtual Devices::SAA::InterpolationType Interpolation() const {
        return Devices::SAA::INTERPOLATION_LQ;     // défaut zxtune pour la SAA
    }
};

class Fifo : public Sound::Receiver {
public:
    virtual void ApplyData(const Sound::Chunk::Ptr& data) {
        if (data) samples.insert(samples.end(), data->begin(), data->end());
    }
    virtual void Flush() {}
    std::vector<Sound::Sample> samples;
    size_t head = 0;
    size_t available() const { return samples.size() - head; }
    void compact() {
        if (head > 65536 && head * 2 > samples.size()) {
            samples.erase(samples.begin(), samples.begin() + (ptrdiff_t)head);
            head = 0;
        }
    }
    void clear() { samples.clear(); head = 0; }
};

struct CopDecoder {
    std::vector<uint8_t>            img;
    bool                            familyB = false;
    int                             song = 0;
    Machine*                        m = nullptr;
    boost::shared_ptr<Fifo>         fifo;
    Devices::SAA::Chip::Ptr         chip;
    int64_t                         frame = 0;       // trames émulées depuis le début
    bool                            fullRegs = true; // envoyer TOUS les registres
    bool                            ended = false;
    int                             passFrames = 0;  // une passe, en trames 50 Hz
    bool                            stops = false;   // le morceau s'arrête de lui-même
    int                             passesToPlay = 1;// 0 = sans fin
    uint64_t                        lengthFrames = 0;// une passe, en trames de sortie
    uint64_t                        produced = 0;    // trames de sortie rendues
    // Vitesse native: la trame du programme Z80 (l'appel de #8000) dure
    // 20 ms ÷ vitesse en SORTIE; la SAA, elle, garde son horloge — hauteur
    // conservée. `frame` reste en trames de MORCEAU.
    double                          speed = 1.0;
    double                          stampUs = 0.0;   // horodatage SAA (sortie)

    ~CopDecoder() { delete m; }
};

inline CopDecoder* cd(RewampDecoder* d) { return reinterpret_cast<CopDecoder*>(d); }

int64_t end_frame(const CopDecoder* d) {
    if (d->passFrames <= 0) return -1;                         // inconnue: sans fin
    if (d->stops) return d->passFrames;
    if (d->passesToPlay <= 0) return -1;
    return (int64_t)d->passFrames * d->passesToPlay;
}

void publish_notes(const Machine* m) {
    const bool on = (m->saa[0x1c] & 1) != 0;
    for (int v = 0; v < kVoices; v++) {
        const uint8_t lvl = m->saa[v];
        const int vol = (lvl & 0x0f) > (lvl >> 4) ? (lvl & 0x0f) : (lvl >> 4);
        const bool tone = (m->saa[0x14] >> v) & 1;
        const uint8_t octReg = m->saa[0x10 + v / 2];
        const int oct = (v & 1) ? ((octReg >> 4) & 7) : (octReg & 7);
        const int n = m->saa[0x08 + v];
        const bool sounding = on && tone && vol > 0;
        /* Fiche technique SAA1099, horloge 8 MHz: f = 15625·2^oct / (511 − n). */
        vgm_last_note[v] = sounding ? (int)lrint(15625.0 * (1 << oct) / (511 - n)) : 0;
        vgm_last_vol[v]  = sounding ? (unsigned char)(vol * 4) : 0;
    }
}

/* Émule une trame et la remet à la puce, qui rend jusqu'à son horodatage. */
bool step_frame(CopDecoder* d) {
    const int64_t endAt = end_frame(d);
    if (endAt >= 0 && d->frame >= endAt) { d->ended = true; return false; }
    if (!d->m->frame()) { d->ended = true; return false; }
    d->frame++;

    Devices::SAA::DataChunk chunk;
    d->stampUs += (1000000.0 / kFrameHz) / d->speed;
    chunk.TimeStamp = Devices::SAA::Stamp((uint64_t)llround(d->stampUs));
    for (int r = 0; r < Devices::SAA::Registers::TOTAL; r++)
        chunk.Data.Data[r] = d->m->saa[r];
    chunk.Data.Mask = d->fullRegs ? 0x0fffffffu : (d->m->touched & 0x0fffffffu);
    /* Le registre 0x1C (bit 0 = sortie active) n'existe pas dans le modèle
     * zxtune: une sortie coupée s'y traduit par des niveaux nuls. */
    if (!(d->m->saa[0x1c] & 1)) {
        for (int v = 0; v < kVoices; v++) chunk.Data.Data[v] = 0;
        chunk.Data.Mask |= 0x3f;
    } else if (d->m->touched & (1u << 0x1c)) {
        chunk.Data.Mask |= 0x3f;                               // niveaux ré-émis au réveil
    }
    d->fullRegs = false;
    d->chip->RenderData(chunk);
    publish_notes(d->m);
    return true;
}

/* Remet machine et puce au début du morceau, sans rien rendre. */
void restart(CopDecoder* d) {
    d->m->load(d->img, d->familyB, d->song);
    d->chip->Reset();
    d->fifo->clear();
    d->frame = 0;
    d->fullRegs = true;
    d->ended = false;
    d->produced = 0;
    d->stampUs = 0.0;
}

/* ── vtable ──────────────────────────────────────────────────────────────── */

int cop_probe(const char* ext, const uint8_t* hdr, size_t n) {
    if (!cop_name(ext)) return 0;
    /* Le lecteur « B » se reconnaît dans l'en-tête: au-dessus des 55 de zxtune. */
    if (hdr && is_family_b(hdr, n)) return 100;
    return 0;
}

/* Le générique ne se reconnaît qu'à l'ABSENCE d'E-Tracker, donc sur le fichier
 * entier: un .cop E-Tracker reste à zxtune (55), un programme Z80 brut vient ici. */
int cop_probe_path(const char* ext, const uint8_t* hdr, size_t n,
                   const char* path, uint64_t fileSize) {
    const int s = cop_probe(ext, hdr, n);
    if (s || !cop_name(ext) || !path || fileSize == 0 || fileSize > kMaxImage) return s;
    char clean[4096];
    split_subsong(path, clean, sizeof(clean));
    std::vector<uint8_t> img;
    if (!read_image(clean, img)) return 0;
    return has_etracker(img.data(), img.size()) ? 0 : 70;
}

void cop_close(RewampDecoder* h) { delete cd(h); }

RewampDecoder* cop_open(const char* path, RewampAudioFormat* outFormat) {
    if (!path) return nullptr;
    char clean[4096];
    const int sub = split_subsong(path, clean, sizeof(clean));
    std::vector<uint8_t> img;
    if (!read_image(clean, img)) return nullptr;
    if (has_etracker(img.data(), img.size())) return nullptr;   // à zxtune

    CopDecoder* d = new CopDecoder();
    d->img.swap(img);
    d->familyB = is_family_b(d->img.data(), d->img.size());
    const int songs = d->familyB ? count_songs_b(d->img) : 1;
    if (songs <= 0) { delete d; return nullptr; }
    d->song = sub < songs ? sub : 0;

    d->passFrames = measure_pass(d->img, d->familyB, d->song, &d->stops);
    d->lengthFrames = (uint64_t)d->passFrames * kSamplesPerFrm;
    /* Boucle native seulement pour un morceau qui REBOUCLE et dont la passe est
     * connue; un jingle qui s'arrête relève du repli générique. */
    if (d->passFrames == 0 || d->stops) g_force_loop_native_veto = 1;
    if (g_force_loop_mode == 2)      d->passesToPlay = 0;
    else if (g_force_loop_mode == 1) d->passesToPlay = (g_force_loop_count < 0 ? 0 : g_force_loop_count) + 1;
    else                             d->passesToPlay = 1;

    d->m = new Machine();
    d->fifo = boost::make_shared<Fifo>();
    d->chip = Devices::SAA::CreateChip(boost::make_shared<ChipParams>(), d->fifo);

    rewamp_channel_data_reset(kVoices);                   // AVANT le premier rendu
    rewamp_channel_data_set_ring_write_size(SOUND_BUFFER_SIZE_SAMPLE * 4 * 2);
    rewamp_channel_data_set_ring_circular(1);
    rewamp_voices_meta_reset();
    rewamp_voices_add_chip("SAA1099", 0, kVoices);
    for (int v = 0; v < kVoices; v++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "SAA %d", v + 1);
        rewamp_voice_set_name(v, nm);
    }
    restart(d);

    rewamp_track_message_append("Format: SAM Coupé COP (Z80 player code)\n");
    rewamp_track_message_append("Player: %s, called every frame (50 Hz) at #8000\n",
                                d->familyB ? "song number at #8001" : "single tune");
    if (d->familyB) rewamp_track_message_append("Songs: %d\n", songs);
    rewamp_track_message_append("Chip: Philips SAA1099, 8 MHz (zxtune)\n");

    if (outFormat) {
        outFormat->channels = 2;
        outFormat->sampleRate = kRate;
    }
    return reinterpret_cast<RewampDecoder*>(d);
}

uint64_t cop_read(RewampDecoder* h, float* out, uint64_t frameCount) {
    CopDecoder* d = cd(h);
    if (!d || !out || frameCount == 0) return 0;
    while (d->fifo->available() < frameCount && !d->ended) step_frame(d);
    uint64_t n = d->fifo->available();
    if (n > frameCount) n = frameCount;
    const Sound::Sample* s = d->fifo->samples.data() + d->fifo->head;
    const float k = 1.0f / 32768.0f;
    for (uint64_t i = 0; i < n; i++) {
        out[i * 2]     = (float)s[i].Left() * k;
        out[i * 2 + 1] = (float)s[i].Right() * k;
    }
    d->fifo->head += (size_t)n;
    d->fifo->compact();
    d->produced += n;
    return n;
}

/* Rejoue depuis le début en faisant tourner la PUCE aussi, échantillons jetés:
 * ses oscillateurs, son bruit et ses enveloppes ont une phase que remettre la
 * puce à zéro perdrait (écart mesuré jusqu'à 0,18 contre le rendu continu).
 * Coût: la SAA rend ~40× plus vite que le temps réel. */
void cop_seek(RewampDecoder* h, uint64_t frameIndex) {
    CopDecoder* d = cd(h);
    if (!d) return;
    restart(d);
    // Cible en temps du MORCEAU; depuis le début à vitesse constante, elle
    // tombe à cible ÷ vitesse en sortie.
    uint64_t skip = (uint64_t)llround((double)frameIndex / d->speed);
    while (skip > 0 && !d->ended) {
        while (d->fifo->available() == 0 && !d->ended) step_frame(d);
        uint64_t n = d->fifo->available();
        if (n > skip) n = skip;
        d->fifo->head += (size_t)n;
        d->produced += n;
        skip -= n;
        d->fifo->compact();
    }
}

uint64_t cop_length(RewampDecoder* h) { return h ? cd(h)->lengthFrames : 0; }

void cop_configure_loop(RewampDecoder* h, int mode, int count) {
    CopDecoder* d = cd(h);
    if (!d) return;
    if (mode == 2)      d->passesToPlay = 0;
    else if (mode == 1) d->passesToPlay = (count < 0 ? 0 : count) + 1;
    else                d->passesToPlay = 1;
}

/* Vitesse NATIVE: la trame du Z80 raccourcit, la SAA garde son horloge. */
int cop_set_speed(RewampDecoder* h, double speed) {
    CopDecoder* d = cd(h);
    if (!d || speed < 0.2 || speed > 3.0) return 0;
    d->speed = speed;
    return 1;
}

const RewampPluginVTable kSamCopVTable = {
    "SAM Coupé COP",
    cop_probe,
    cop_open,
    cop_read,
    cop_seek,
    cop_length,
    cop_close,
    cop_configure_loop,
    0,          /* supportsNativeFadeout: la fenêtre de fondu générique s'applique */
    nullptr,    /* engine_id */
    nullptr,    /* param_changed */
    nullptr,    /* pattern_song_info */
    nullptr,    /* pattern_order */
    nullptr,    /* pattern_num_rows */
    nullptr,    /* pattern_get */
    nullptr,    /* pattern_cursor */
    cop_probe_path,
    1,          /* noPrefixProbe: pas de forme préfixe Amiga pour un .cop */
    cop_set_speed,
};

std::vector<int> g_probe_ms;

}  // namespace

extern "C" const RewampPluginVTable* rewamp_samcop_plugin(void) { return &kSamCopVTable; }

/* Sous-chansons d'un .cop de la famille « B » (0 sinon: un E-Tracker garde le
 * compte de zxtune, un générique n'a qu'un morceau). Instance PROPRE: aucune
 * donnée de voix touchée. */
extern "C" int rewamp_samcop_probe_subsong_count(const char* path) {
    g_probe_ms.clear();
    if (!path) return 0;
    char clean[4096];
    split_subsong(path, clean, sizeof(clean));
    const char* dot = strrchr(clean, '.');
    if (!dot || !cop_name(dot + 1)) return 0;
    std::vector<uint8_t> img;
    if (!read_image(clean, img) || !is_family_b(img.data(), img.size())) return 0;
    const int n = count_songs_b(img);
    for (int i = 0; i < n; i++) {
        bool stops = false;
        const int f = measure_pass(img, true, i, &stops);
        g_probe_ms.push_back(f > 0 ? f * (1000 / kFrameHz) : -1);
    }
    return n;
}

extern "C" int rewamp_samcop_probe_get_duration_ms(int idx) {
    if (idx < 0 || idx >= (int)g_probe_ms.size()) return -1;
    return g_probe_ms[(size_t)idx];
}

#endif /* REWAMP_WITH_ZXTUNE */
