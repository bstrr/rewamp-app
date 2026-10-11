/* Ron Klaren (Amiga) — greffon autour du portage C++ du lecteur de
 * NostalgicPlayer (third_party/ronklaren, MIT, © Thomas Neumann).
 *
 * Pourquoi un moteur à part, alors que UADE lit déjà `.rk`: UADE confie ces
 * fichiers au player générique CustomMade (`eagleplayer.conf`: `CustomMade
 * prefixes=cm,rk,rkb ignore_player_check`), qui ne sait jouer que la forme de
 * la routine Ron Klaren qui s'appelle ELLE-MÊME depuis son interruption CIA.
 * « electricity.rk » (modland) est l'autre forme: trois sauts en tête au lieu
 * de deux, aucune interruption, un `play` à appeler à chaque trame. Sous UADE
 * il annonçait 2 418 sous-chansons et ne rendait que du silence.
 * NostalgicPlayer ne fait pas tourner le 68000: il LIT dans le code les
 * adresses des tables (sous-chansons, vitesse, instruments, arpèges…) puis
 * rejoue les données avec sa propre routine — ce qui couvre les deux formes.
 *
 *  - Reconnaissance: extension `rk` (suffixe OU préfixe Amiga `RK.nom`), hunk
 *    exécutable `00 00 03 F3` et « RON_KLAREN_SOUNDMODULE! » à l'octet 40 —
 *    le test de NostalgicPlayer. Score 100: au-dessus des 58 d'UADE sur la
 *    seule extension, sous REWAMP_SCORE_EXCLUSIVE (une épingle reste
 *    souveraine). Sans la marque, le fichier reste à UADE (`.cm`, `.rkb`, un
 *    `.rk` d'une autre famille).
 *  - Paula: 4 voix émulées ici (pas de bibliothèque): période → fréquence
 *    3 546 895 / période (plancher 113, comme le mixeur NostalgicPlayer),
 *    volume 0-64, échantillons 8 bits signés, interpolation linéaire.
 *    Stéréo Amiga: voies 0 et 3 à gauche, 1 et 2 à droite, puis le MÊME
 *    rapprochement que le moteur UADE (réglages « uade » pan_enabled /
 *    pan_value, défaut 0,7): un module Amiga sonne pareil quel que soit le
 *    moteur qui le joue.
 *  - Rythme: un appel au lecteur par tick, à la fréquence qu'il annonce
 *    (709 379 / valeur CIA trouvée dans le code, 50 Hz pour la forme VBlank).
 *  - Fin: le lecteur signale la fin quand chaque voix a bouclé sa liste de
 *    positions (ou sur une commande « fin de morceau ») — comme dans
 *    NostalgicPlayer. La durée d'UN passage se mesure à l'ouverture en
 *    simulant les ticks sans mixer.
 *  - Boucle: NATIVE (configure_loop). Le morceau reboucle de lui-même chez
 *    Ron Klaren — continuer à jouer après la fin EST le passage suivant, sans
 *    le trou d'un rechargement. Mode 0 = s'arrêter à la première fin, 1 = au
 *    bout de count+1 fins, 2 = jamais. Un morceau dont aucune fin n'est
 *    trouvée en 60 min simulées oppose son veto: le repli générique Dart
 *    prend la main.
 *
 * Compilé seulement si REWAMP_WITH_RONKLAREN est défini. */
#ifdef REWAMP_WITH_RONKLAREN

#include "rewamp_plugin.h"
#include "rewamp_channel_data.h"
#include "ModizerVoicesData.h"

#include "../third_party/ronklaren/RonKlarenWorker.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vector>

extern "C" int g_force_loop_native_veto;

namespace {

const int      kRate       = 44100;
const int      kVoices     = 4;
const int      kRingSize   = SOUND_BUFFER_SIZE_SAMPLE * 4 * 2;   // 4096, PLUGINS.md §2.3
const uint32_t kPaulaClock = 3546895;                            // PAL
const double   kMaxSimSecs = 60.0 * 60.0;                        // garde-fou de la mesure
const size_t   kMaxFile    = 16u * 1024u * 1024u;
/* Une voix: échantillon 8 bits × volume 64 = ±8192; deux voix par côté. */
const float    kGain       = 1.0f / 16384.0f;

/* ── Paula: une voix ───────────────────────────────────────────────────── */

struct PaulaVoice final : public ronklaren::IChannel {
    const int8_t* data = nullptr;
    uint32_t end = 0;           // fin de la partie jouée (octets)
    uint32_t loopStart = 0;
    uint32_t loopLen = 0;       // 0 = un seul passage
    double   pos = 0.0;
    bool     active = false;
    uint32_t period = 0;
    uint32_t freq = 0;
    uint16_t vol = 0;
    int16_t  sampleNumber = 0;

    void Reset() { *this = PaulaVoice(); }

    void PlaySample(int16_t num, const int8_t* d, uint32_t start, uint32_t length) override {
        sampleNumber = num;
        if (!d || length == 0) { active = false; data = nullptr; return; }
        data = d;
        end = start + length;
        pos = start;
        loopStart = 0;
        loopLen = 0;
        active = true;
    }
    void SetLoop(uint32_t start, uint32_t length) override {
        if (!data || length == 0 || start >= end) return;
        if (start + length > end) length = end - start;
        loopStart = start;
        loopLen = length;
    }
    void SetAmigaPeriod(uint32_t p) override {
        period = p;
        if (p != 0) {
            if (p < 113) p = 113;           // Paula ne descend pas plus bas
            freq = kPaulaClock / p;
        } else {
            freq = 0;
        }
    }
    void SetAmigaVolume(uint16_t v) override { vol = v > 64 ? 64 : v; }
    void Mute() override { active = false; }

    double Step() const { return (double)freq / (double)kRate; }

    /* Fin de la partie jouée atteinte: boucle ou arrêt. */
    inline void Wrap() {
        if (pos < (double)end) return;
        if (loopLen == 0) { active = false; return; }
        pos = (double)loopStart + fmod(pos - (double)end, (double)loopLen);
    }

    /* Échantillon de sortie suivant, avant volume (−128..127).
     *
     * Paula tient chaque octet jusqu'au suivant (bloqueur d'ordre zéro): on
     * rend la MOYENNE de ce signal en escalier sur l'intervalle d'une frame de
     * sortie. Quand la voix est plus lente que 44,1 kHz (le cas courant) c'est
     * l'escalier tel quel — le timbre brillant d'un Amiga et de NostalgicPlayer
     * (dont le mixeur, par défaut, n'interpole pas); quand elle est plus
     * rapide, la moyenne évite le crénelage d'une lecture au plus proche. Une
     * interpolation linéaire, essayée d'abord, éteignait les aigus (centroïde
     * spectral 2,3 kHz contre 4,2 kHz pour uade123 sur « astra 1 »). */
    inline float Next() {
        if (!active || freq == 0) return 0.0f;
        const double step = Step();
        double remaining = step;
        double acc = 0.0;
        while (remaining > 1e-12 && active) {
            const uint32_t i = (uint32_t)pos;
            double seg = (double)(i + 1) - pos;
            if (seg > remaining) seg = remaining;
            acc += (double)data[i] * seg;
            pos += seg;
            remaining -= seg;
            Wrap();
        }
        return (float)(acc / step);
    }

    /* Avance de n échantillons sans rien produire (seek). */
    void Advance(uint64_t n) {
        if (!active || freq == 0 || n == 0) return;
        pos += Step() * (double)n;
        Wrap();
    }
};

/* Canal muet, pour mesurer la durée sans toucher aux voix qui jouent. */
struct NullChannel final : public ronklaren::IChannel {
    void PlaySample(int16_t, const int8_t*, uint32_t, uint32_t) override {}
    void SetLoop(uint32_t, uint32_t) override {}
    void SetAmigaPeriod(uint32_t) override {}
    void SetAmigaVolume(uint16_t) override {}
    void Mute() override {}
};

bool read_file(const char* path, std::vector<uint8_t>& out) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || (size_t)len > kMaxFile) { fclose(fp); return false; }
    out.resize((size_t)len);
    const bool ok = fread(out.data(), 1, (size_t)len, fp) == (size_t)len;
    fclose(fp);
    return ok;
}

/* `chemin?subsong=N` → chemin nu + N (0 si absent). */
int split_subsong(const char* path, char* clean, size_t cap) {
    strncpy(clean, path, cap - 1);
    clean[cap - 1] = '\0';
    int sub = 0;
    char* q = strrchr(clean, '?');
    if (q && strncmp(q, "?subsong=", 9) == 0) { sub = atoi(q + 9); *q = '\0'; }
    return sub < 0 ? 0 : sub;
}

/* Échantillons de sortie du prochain tick, accumulateur fractionnaire: la même
 * règle pour la mesure, la lecture et le seek, donc des frames qui tombent
 * juste entre les trois. */
inline uint64_t tick_frames(double& acc, double perTick) {
    acc += perTick;
    const uint64_t n = (uint64_t)acc;
    acc -= (double)n;
    return n;
}

/* Durée d'UN passage de la sous-chanson, en frames; 0 si aucune fin n'est
 * trouvée sous kMaxSimSecs. Laisse le lecteur réinitialisé sur `sub`. */
uint64_t measure_pass(ronklaren::RonKlarenWorker& w, int sub) {
    NullChannel nc[kVoices];
    ronklaren::IChannel* const chp[kVoices] = { &nc[0], &nc[1], &nc[2], &nc[3] };
    w.InitSound(sub);
    const double perTick = (double)kRate / (double)w.PlayingFrequency();
    const uint64_t maxTicks = (uint64_t)(kMaxSimSecs * (double)w.PlayingFrequency());
    double acc = 0.0;
    uint64_t frames = 0, result = 0;
    for (uint64_t t = 0; t < maxTicks; t++) {
        w.Play(chp);
        if (w.HasEndReached) { result = frames; break; }
        frames += tick_frames(acc, perTick);
    }
    w.InitSound(sub);
    return result;
}

}  // namespace

/* ── Décodeur ──────────────────────────────────────────────────────────── */

/* ⚠️ PAS `struct RewampDecoder`: ce type a un constructeur et un destructeur
 * (vecteurs du lecteur), émis en symboles FAIBLES globaux — et d'autres greffons
 * C++ définissent eux aussi un `struct RewampDecoder`, d'une autre forme.
 * L'éditeur de liens n'en garderait qu'un pour tous (violation de l'ODR). Le
 * type vit donc dans un espace anonyme, et le pointeur opaque n'est qu'un
 * transtypage. */
namespace {

struct RkDecoder {
    ronklaren::RonKlarenWorker w;
    PaulaVoice ch[kVoices];
    ronklaren::IChannel* chp[kVoices];
    int      subsong = 0;
    double   perTick = 882.0;
    double   acc = 0.0;          // accumulateur fractionnaire des ticks
    uint64_t pending = 0;        // frames restant à mixer pour le tick courant
    uint64_t lengthFrames = 0;   // un passage, 0 = inconnu
    int      endsToPlay = 1;     // 0 = jamais de fin (boucle infinie)
    int      endsSeen = 0;
    bool     ended = false;
    float    pan = 0.35f;        // 0 = séparation Amiga totale, 0,5 = mono
    // Vitesse native: un tick du lecteur dure taux/fréquence ÷ vitesse en
    // SORTIE; la Paula avance en temps réel — hauteur conservée.
    double   speed = 1.0;
};

inline RkDecoder* rk(RewampDecoder* d) { return reinterpret_cast<RkDecoder*>(d); }

void rk_restart(RkDecoder* d) {
    d->w.InitSound(d->subsong);
    for (int v = 0; v < kVoices; v++) d->ch[v].Reset();
    d->perTick = (double)kRate / (double)d->w.PlayingFrequency() / d->speed;
    d->acc = 0.0;
    d->pending = 0;
    d->endsSeen = 0;
    d->ended = false;
}

/* Un tick du lecteur. Rend false si le morceau s'arrête ici. */
bool rk_tick(RkDecoder* d) {
    d->w.Play(d->chp);
    if (d->w.HasEndReached) {
        d->w.HasEndReached = false;
        d->endsSeen++;
        if (d->endsToPlay > 0 && d->endsSeen >= d->endsToPlay) {
            d->ended = true;
            return false;
        }
    }
    d->pending = tick_frames(d->acc, d->perTick);
    return true;
}

void rk_update_vu(RkDecoder* d) {
    for (int v = 0; v < kVoices; v++) {
        const PaulaVoice& c = d->ch[v];
        const bool on = c.active && c.freq != 0 && c.vol != 0 && !d->ended;
        vgm_last_note[v]  = on ? c.freq : 0;               // Hz (période → fréquence)
        vgm_last_vol[v]   = on ? c.vol : 0;
        vgm_last_instr[v] = (unsigned char)(c.sampleNumber & 0xff);
    }
}

/* Mixe n frames dans out (stéréo entrelacé), et les anneaux d'oscilloscope. */
void rk_mix(RkDecoder* d, float* out, uint64_t n) {
    const int64_t mute = generic_mute_mask;
    for (uint64_t i = 0; i < n; i++) {
        float vs[kVoices];
        for (int v = 0; v < kVoices; v++) {
            float s = d->ch[v].Next() * (float)d->ch[v].vol;   // ±8192
            if (mute & (1LL << v)) s = 0.0f;                     // vrai mixage coupé
            vs[v] = s;
            if (m_voice_buff[v]) {
                const int sc = (int)(s * (1.0f / 64.0f));
                m_voice_buff[v][(m_voice_current_ptr[v] >> MODIZER_OSCILLO_OFFSET_FIXEDPOINT)
                                & (kRingSize - 1)] = (signed char)LIMIT8(sc);
            }
            m_voice_current_ptr[v] += 1LL << MODIZER_OSCILLO_OFFSET_FIXEDPOINT;
        }
        const float l = vs[0] + vs[3];
        const float r = vs[1] + vs[2];
        const float m = (r - l) * d->pan;
        out[2 * i]     = (l + m) * kGain;
        out[2 * i + 1] = (r - m) * kGain;
    }
}

int rk_probe(const char* ext, const uint8_t* header, size_t headerSize) {
    if (!ext || strcasecmp(ext, "rk") != 0) return 0;
    if (!header || headerSize < 63) return 0;
    if (header[0] != 0x00 || header[1] != 0x00 || header[2] != 0x03 || header[3] != 0xf3) return 0;
    if (memcmp(header + 40, "RON_KLAREN_SOUNDMODULE!", 23) != 0) return 0;
    return 100;
}

void rk_close(RewampDecoder* h) { delete rk(h); }

RewampDecoder* rk_open(const char* path, RewampAudioFormat* outFormat) {
    if (!path) return nullptr;
    char clean[4096];
    const int sub = split_subsong(path, clean, sizeof(clean));
    std::vector<uint8_t> file;
    if (!read_file(clean, file)) return nullptr;
    if (!ronklaren::RonKlarenWorker::Identify(file.data(), file.size())) return nullptr;

    RkDecoder* d = new RkDecoder();
    if (!d->w.Load(file.data(), file.size())) { delete d; return nullptr; }
    for (int v = 0; v < kVoices; v++) d->chp[v] = &d->ch[v];
    d->subsong = sub < d->w.NumberOfSubSongs() ? sub : 0;

    {
        const int panOn = (int)rewamp_get_engine_param("uade", "pan_enabled", 1);
        double pv = rewamp_get_engine_param("uade", "pan_value", 0.7);
        if (pv < 0.0) pv = 0.0;
        if (pv > 2.0) pv = 2.0;
        d->pan = panOn ? (float)(pv / 2.0) : 0.0f;   // même échelle que uade_effect_pan
    }

    d->lengthFrames = measure_pass(d->w, d->subsong);
    if (d->lengthFrames == 0) g_force_loop_native_veto = 1;   // pas de fin trouvée
    rk_restart(d);

    rewamp_channel_data_reset(kVoices);                 // AVANT le premier tick
    rewamp_channel_data_set_ring_write_size(kRingSize);
    rewamp_voices_meta_reset();
    rewamp_voices_add_chip("Paula", 0, kVoices);
    for (int v = 0; v < kVoices; v++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "Paula %d", v + 1);
        rewamp_voice_set_name(v, nm);
    }

    {
        const ronklaren::RonKlarenWorker& w = d->w;
        rewamp_track_message_append("Format: Ron Klaren (Amiga, NostalgicPlayer replay)\n");
        if (w.IsVBlankPlayer())
            rewamp_track_message_append("Player: VBlank (play called every frame), %.2f Hz\n",
                                        (double)w.PlayingFrequency());
        else
            rewamp_track_message_append("Player: CIA interrupt, timer %u, %.2f Hz\n",
                                        (unsigned)w.CiaValue(), (double)w.PlayingFrequency());
        rewamp_track_message_append("Subsong: %d / %d\n", d->subsong + 1, w.NumberOfSubSongs());
        rewamp_track_message_append("Positions: %d, %d, %d, %d\n",
                                    w.PositionListLength(0), w.PositionListLength(1),
                                    w.PositionListLength(2), w.PositionListLength(3));
        rewamp_track_message_append("Tracks: %d · Instruments: %d\n",
                                    w.NumberOfTracks(), w.NumberOfInstruments());
        if (d->lengthFrames) {
            const unsigned secs = (unsigned)(d->lengthFrames / kRate);
            rewamp_track_message_append("Duration: %u:%02u\n", secs / 60, secs % 60);
        }
    }

    if (outFormat) {
        outFormat->channels = 2;
        outFormat->sampleRate = kRate;
    }
    return reinterpret_cast<RewampDecoder*>(d);
}

uint64_t rk_read(RewampDecoder* h, float* out, uint64_t frameCount) {
    RkDecoder* d = rk(h);
    if (!d || !out || frameCount == 0) return 0;
    uint64_t done = 0;
    while (done < frameCount && !d->ended) {
        if (d->pending == 0) {
            if (!rk_tick(d)) break;
            continue;
        }
        uint64_t n = frameCount - done;
        if (n > d->pending) n = d->pending;
        rk_mix(d, out + done * 2, n);
        d->pending -= n;
        done += n;
    }
    rk_update_vu(d);
    return done;
}

void rk_seek(RewampDecoder* h, uint64_t frameIndex) {
    RkDecoder* d = rk(h);
    if (!d) return;
    rk_restart(d);
    /* Rejoue les ticks sans mixer; les voix avancent d'un bloc par tick. Cible
     * en temps du MORCEAU ⇒ ÷ vitesse en sortie (vitesse constante depuis le
     * début). */
    frameIndex = (uint64_t)llround((double)frameIndex / d->speed);
    uint64_t at = 0;
    while (at < frameIndex && !d->ended) {
        if (d->pending == 0) {
            if (!rk_tick(d)) break;
            continue;
        }
        uint64_t n = frameIndex - at;
        if (n > d->pending) n = d->pending;
        for (int v = 0; v < kVoices; v++) d->ch[v].Advance(n);
        d->pending -= n;
        at += n;
    }
    rk_update_vu(d);
}

uint64_t rk_length(RewampDecoder* h) { return h ? rk(h)->lengthFrames : 0; }

/* Boucle native: le lecteur reboucle de lui-même, on ne fait que compter les
 * fins. count = répétitions APRÈS le premier passage. */
void rk_configure_loop(RewampDecoder* h, int mode, int count) {
    RkDecoder* d = rk(h);
    if (!d) return;
    if (mode == 2) d->endsToPlay = 0;
    else if (mode == 1) d->endsToPlay = (count < 0 ? 0 : count) + 1;
    else d->endsToPlay = 1;
}

/* Vitesse NATIVE (voir RkDecoder.speed). */
int rk_set_speed(RewampDecoder* h, double speed) {
    RkDecoder* d = rk(h);
    if (!d || speed < 0.2 || speed > 3.0) return 0;
    d->speed = speed;
    d->perTick = (double)kRate / (double)d->w.PlayingFrequency() / speed;
    return 1;
}

const RewampPluginVTable kRonKlarenVTable = {
    "Ron Klaren",
    rk_probe,
    rk_open,
    rk_read,
    rk_seek,
    rk_length,
    rk_close,
    rk_configure_loop,
    0,          /* supportsNativeFadeout: la fenêtre de fondu générique s'applique */
    nullptr,    /* engine_id */
    nullptr,    /* param_changed */
    nullptr,    /* pattern_song_info */
    nullptr,    /* pattern_order */
    nullptr,    /* pattern_num_rows */
    nullptr,    /* pattern_get */
    nullptr,    /* pattern_cursor */
    nullptr,    /* probe_path */
    0,          /* noPrefixProbe: `RK.nom` (forme préfixe) doit être reconnu */
    rk_set_speed,
};

/* ── Sonde des sous-chansons (rewamp_probe_subsong_count) ─────────────── */

std::vector<int> g_probe_ms;

}  // namespace

extern "C" const RewampPluginVTable* rewamp_ronklaren_plugin(void) { return &kRonKlarenVTable; }

/* Compte les sous-chansons et mesure chacune; 0 si ce n'est pas un module Ron
 * Klaren que ce lecteur sait charger. Instance PROPRE (aucun état partagé avec
 * le décodeur qui joue, aucune donnée de voix touchée). */
extern "C" int rewamp_ronklaren_probe_subsong_count(const char* path) {
    g_probe_ms.clear();
    if (!path) return 0;
    char clean[4096];
    split_subsong(path, clean, sizeof(clean));
    /* Même règle de NOM que rk_probe (suffixe `.rk` ou préfixe Amiga `rk.`):
     * un `.cm` qui porterait la marque reste à UADE, il ne doit pas recevoir
     * un compte qui n'est pas celui du moteur qui le joue. */
    {
        const char* slash = strrchr(clean, '/');
        const char* name = slash ? slash + 1 : clean;
        const size_t nl = strlen(name);
        const bool suffix = nl > 3 && strcasecmp(name + nl - 3, ".rk") == 0;
        const bool prefix = nl > 3 && strncasecmp(name, "rk.", 3) == 0;
        if (!suffix && !prefix) return 0;
    }
    /* Appelée pour CHAQUE fichier ouvert: l'en-tête d'abord, le fichier
     * entier seulement s'il porte la marque. */
    {
        uint8_t hdr[64];
        FILE* fp = fopen(clean, "rb");
        if (!fp) return 0;
        const size_t hn = fread(hdr, 1, sizeof(hdr), fp);
        fclose(fp);
        if (hn < 63 || hdr[0] != 0 || hdr[1] != 0 || hdr[2] != 0x03 || hdr[3] != 0xf3 ||
            memcmp(hdr + 40, "RON_KLAREN_SOUNDMODULE!", 23) != 0)
            return 0;
    }
    std::vector<uint8_t> file;
    if (!read_file(clean, file)) return 0;
    if (!ronklaren::RonKlarenWorker::Identify(file.data(), file.size())) return 0;
    ronklaren::RonKlarenWorker w;
    if (!w.Load(file.data(), file.size())) return 0;
    const int n = w.NumberOfSubSongs();
    for (int i = 0; i < n; i++) {
        const uint64_t f = measure_pass(w, i);
        g_probe_ms.push_back(f ? (int)(f * 1000 / kRate) : -1);
    }
    return n;
}

extern "C" int rewamp_ronklaren_probe_get_duration_ms(int idx) {
    if (idx < 0 || idx >= (int)g_probe_ms.size()) return -1;
    return g_probe_ms[(size_t)idx];
}

#endif /* REWAMP_WITH_RONKLAREN */
