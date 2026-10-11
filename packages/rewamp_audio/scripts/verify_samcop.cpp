// Harnais du greffon SAM Coupé COP (src/rewamp_plugin_samcop.cpp: code Z80 du
// fichier exécuté par z80ex, son par la SAA1099 de zxtune).
//   verify_samcop <fichier.cop> [secondes=30] [dossier=.]
// Sonde (en-tête puis chemin, comme le registre), sous-chansons, rendu WAV,
// RMS global et par voix, oscilloscope, seek, fin réelle, boucle native.
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern "C" {
#include "rewamp_plugin.h"
#include "rewamp_channel_data.h"
#include "ModizerVoicesData.h"
const RewampPluginVTable* rewamp_samcop_plugin(void);
int rewamp_samcop_probe_subsong_count(const char* path);
int rewamp_samcop_probe_get_duration_ms(int idx);
int g_force_loop_mode = 0;
int g_force_loop_count = 0;
int g_force_loop_native_veto = 0;
double rewamp_get_engine_param(const char*, const char*, double defval) { return defval; }
int64_t rewamp_waveform_pos(void) { return 0; }
int rewamp_waveform_read_i8(int, int8_t*, int) { return 0; }
int rewamp_ext_in_list(const char*, const char* const*) { return 0; }
}

static const int kRate = 44100;

static void write_wav(const std::string& path, const std::vector<float>& pcm) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    const uint32_t frames = (uint32_t)(pcm.size() / 2), data = frames * 4;
    auto p32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto p16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); p32(36 + data); fwrite("WAVEfmt ", 1, 8, f);
    p32(16); p16(1); p16(2); p32(kRate); p32(kRate * 4); p16(4); p16(16);
    fwrite("data", 1, 4, f); p32(data);
    for (float s : pcm) {
        float c = s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
        p16((uint16_t)(int16_t)lrintf(c * 32767.f));
    }
    fclose(f);
}

static double rms_db(const float* p, size_t n) {
    double acc = 0;
    for (size_t i = 0; i < n; i++) acc += (double)p[i] * p[i];
    if (n == 0 || acc <= 0) return -999.0;
    return 10.0 * log10(acc / (double)n);
}

// Rend `frames` frames (ou jusqu'à la fin) dans out; renvoie le nombre rendu.
static uint64_t render(const RewampPluginVTable* vt, RewampDecoder* d, uint64_t frames,
                       std::vector<float>* out) {
    std::vector<float> buf(1024 * 2);
    uint64_t done = 0;
    while (done < frames) {
        uint64_t want = frames - done < 1024 ? frames - done : 1024;
        uint64_t got = vt->read(d, buf.data(), want);
        if (got == 0) break;
        if (out) out->insert(out->end(), buf.begin(), buf.begin() + (long)got * 2);
        done += got;
    }
    return done;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.cop> [seconds=30] [outdir=.]\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];
    const double secs = argc > 2 ? atof(argv[2]) : 30.0;
    const std::string outdir = argc > 3 ? argv[3] : ".";
    rewamp_channel_data_init();
    const RewampPluginVTable* vt = rewamp_samcop_plugin();

    // Sonde: comme le registre (2 Ko d'en-tête, extension en minuscules).
    uint8_t hdr[2048] = {0};
    size_t hn = 0;
    if (FILE* f = fopen(path, "rb")) { hn = fread(hdr, 1, sizeof(hdr), f); fclose(f); }
    const char* dot = strrchr(path, '.');
    std::string ext = dot ? dot + 1 : "";
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    uint64_t fsize = 0;
    if (FILE* f = fopen(path, "rb")) { fseek(f, 0, SEEK_END); fsize = (uint64_t)ftell(f); fclose(f); }
    const int score = vt->probe_path ? vt->probe_path(ext.c_str(), hdr, hn, path, fsize)
                                     : vt->probe(ext.c_str(), hdr, hn);
    int n = rewamp_samcop_probe_subsong_count(path);
    if (n == 0 && score > 0) n = 1;     // générique: un seul morceau
    std::string base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    printf("== %s: probe=%d subsongs=%d\n", base.c_str(), score, n);
    if (score <= 0 || n <= 0) return 1;

    int rc = 0;
    for (int s = 0; s < n; s++) {
        const std::string p = std::string(path) + "?subsong=" + std::to_string(s);
        RewampAudioFormat fmt{};
        g_force_loop_native_veto = 0;
        RewampDecoder* d = vt->open(p.c_str(), &fmt);
        if (!d) { printf("  [%d] open FAILED\n", s); rc = 1; continue; }
        vt->configure_loop(d, 0, 0);
        const uint64_t len = vt->length(d);
        const uint64_t frames = (uint64_t)(secs * kRate);

        generic_mute_mask = 0;
        std::vector<float> pcm;
        const uint64_t got = render(vt, d, frames, &pcm);
        // Oscilloscope: têtes alignées, anneau non vide.
        int64_t p0 = m_voice_current_ptr[0] >> MODIZER_OSCILLO_OFFSET_FIXEDPOINT;
        bool aligned = true, scopeLive = false;
        for (int v = 0; v < 6; v++) {
            if ((m_voice_current_ptr[v] >> MODIZER_OSCILLO_OFFSET_FIXEDPOINT) != p0) aligned = false;
            if (m_voice_buff[v])
                for (int i = 0; i < 4096; i++) if (m_voice_buff[v][i]) { scopeLive = true; break; }
        }
        const double db = rms_db(pcm.data(), pcm.size());
        char wav[1024];
        snprintf(wav, sizeof(wav), "%s/%s_s%d.wav", outdir.c_str(), base.c_str(), s);
        write_wav(wav, pcm);

        // Voix seules.
        double vdb[6];
        for (int v = 0; v < 6; v++) {
            vt->seek(d, 0);
            generic_mute_mask = ~(int64_t)(1LL << v);
            std::vector<float> solo;
            render(vt, d, frames, &solo);
            vdb[v] = rms_db(solo.data(), solo.size());
        }
        generic_mute_mask = 0;

        // Seek: 10 s (ou le milieu), comparé au rendu continu.
        uint64_t at = frames / 3;
        double maxErr = -1;
        if (got > at + 4096) {
            vt->seek(d, at);
            std::vector<float> sk;
            render(vt, d, 4096, &sk);
            maxErr = 0;
            for (size_t i = 0; i < sk.size(); i++) {
                double e = fabs((double)sk[i] - pcm[at * 2 + i]);
                if (e > maxErr) maxErr = e;
            }
        }

        // Fin réelle: tout rendre depuis 0, comparer à length().
        vt->seek(d, 0);
        const uint64_t total = render(vt, d, (uint64_t)kRate * 60 * 30, nullptr);
        vt->close(d);

        // Boucle native: 1 répétition = 2 passages.
        uint64_t total2 = 0;
        if (len) {
            RewampDecoder* d2 = vt->open(p.c_str(), &fmt);
            vt->configure_loop(d2, 1, 1);
            total2 = render(vt, d2, len * 3, nullptr);
            vt->close(d2);
        }

        printf("  [%d] length=%.2fs probe_ms=%d end@%.2fs loop1=%.2fs veto=%d rendered=%.1fs "
               "rms=%.1f dB voices=[%.1f %.1f %.1f %.1f %.1f %.1f] scope=%s%s seekErr=%.4f -> %s\n",
               s, len / (double)kRate, rewamp_samcop_probe_get_duration_ms(s),
               total / (double)kRate, total2 / (double)kRate, g_force_loop_native_veto,
               got / (double)kRate, db, vdb[0], vdb[1], vdb[2], vdb[3], vdb[4], vdb[5],
               scopeLive ? "live" : "EMPTY", aligned ? "" : " MISALIGNED", maxErr, wav);
        /* Le silence se juge au NIVEAU: l'anneau d'oscilloscope ne garde que la fin
         * du rendu, et un jingle qui s'arrête finit sur du silence (prince n°4). */
        if (db < -60.0) { printf("  [%d] SILENT\n", s); rc = rc ? rc : 3; }
    }
    return rc;
}
