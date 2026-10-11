/* Harnais hors ligne du greffon NoiseTracker GS (rewamp_plugin_ntgs.c).
 *   verify_ntgs <module> [secondes] [sortie.wav]
 * Imprime le panneau ⓘ, la durée d'un passage, le RMS global et par voix (lu
 * dans les anneaux d'oscilloscope), exerce seek() et un mute, et écrit un WAV
 * 16 bits stéréo si demandé. Code de sortie 3 si le rendu est silencieux.
 * Voir verify_ntgs.sh. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ModizerVoicesData.h"
#include "rewamp_channel_data.h"
#include "rewamp_plugin.h"

#ifdef NTGS_ORACLE
/* ── Oracle: le greffon compilé au pas du DOC (27118,48 Hz) et ses écritures
 * de registres rejouées dans es5503.c de libvgm (lignée MAME). La somme brute
 * des oscillateurs (avant gain) doit être IDENTIQUE échantillon par
 * échantillon, voix B seulement (les voix A sont un modèle, pas un DOC). */
#define NTGS_STEP_RATE (7159090.0 / 8.0 / 33.0)
static void oracle_w(void* d, void* s, int k, int reg, int val);
#define NTGS_DOCW(d, s, k, reg, val) oracle_w((void*)(d), (void*)(s), (k), (reg), (val))
#include "../src/rewamp_plugin_ntgs.c"
#include "../third_party/libvgm/libvgm/emu/cores/es5503.c"
static ES5503Chip* g_doc;
static int g_live;
static void oracle_w(void* dv, void* sv, int k, int reg, int val) {
    RewampDecoder* d = (RewampDecoder*)dv; NtgsSlot* s = (NtgsSlot*)sv;
    if (!g_live || !g_doc) return;
    int osc = 2 * (s->voice - d->nA) + k;
    es5503_w(g_doc, (UINT8)(reg | osc), (UINT8)val);
}
static int oracle_main(const char* path, double secs) {
    RewampAudioFormat fmt; memset(&fmt, 0, sizeof fmt);
    RewampDecoder* d = ntgs_open(path, &fmt);
    if (!d) { fprintf(stderr, "open failed\n"); return 1; }
    generic_mute_mask = (1LL << d->nA) - 1;   /* oracle = voix B seulement */
    DEV_GEN_CFG cfg; memset(&cfg, 0, sizeof cfg);
    cfg.clock = 7159090; cfg.flags = 2;
    DEV_INFO inf; memset(&inf, 0, sizeof inf);
    if (device_start_es5503(&cfg, &inf)) { fprintf(stderr, "es5503 start failed\n"); return 1; }
    g_doc = (ES5503Chip*)inf.dataPtr;
    m_voice_current_system = 127;            /* pas de hook Modizer */
    memset(g_doc->docram, 0, g_doc->dramsize);
    es5503_write_ram(g_doc, 0, 65536, d->docRam);
    es5503_w(g_doc, 0xe1, 0x3c);
    for (int o = 0; o < 32; o++) es5503_w(g_doc, (UINT8)(0xa0 | o), 1);
    g_live = 1;
    ntgs_restart(d);
    enum { N = 1024 };
    static DEV_SMPL bl[N], br[N]; DEV_SMPL* outs[2] = { bl, br };
    const uint64_t total = (uint64_t)(secs * NTGS_DOC_RATE);
    uint64_t done = 0, diff = 0, nz = 0, first = (uint64_t)-1; double maxd = 0;
    while (done < total && !d->ended) {
        if ((double)d->cur >= d->nextRow) { ntgs_process_row(d, 1); if (d->ended) break; d->nextRow += ntgs_row_len(d->tempo); continue; }
        uint64_t n = total - done; uint64_t toRow = (uint64_t)ceil(d->nextRow - (double)d->cur);
        if (toRow < 1) toRow = 1; if (n > toRow) n = toRow; if (n > N) n = N;
        ntgs_mix(d, NULL, (int)n, 0);
        es5503_pcm_update(g_doc, (UINT32)n, outs);
        for (uint64_t i = 0; i < n; i++) {
            double e = fabs((double)d->mixL[i] - bl[i]) + fabs((double)d->mixR[i] - br[i]);
            if (bl[i] || br[i]) nz++;
            if (e > 0) { diff++; if (first == (uint64_t)-1) first = done + i; if (e > maxd) maxd = e; }
        }
        d->cur += n; done += n;
    }
    printf("oracle %s: %llu samples @27118 Hz, non-silent=%llu, differing=%llu (first at %.3fs), max |err|=%.0f\n",
           path, (unsigned long long)done, (unsigned long long)nz, (unsigned long long)diff,
           first == (uint64_t)-1 ? -1.0 : first / NTGS_DOC_RATE, maxd);
    device_stop_es5503(g_doc);
    ntgs_close(d);
    return diff ? 4 : 0;
}
#else
const RewampPluginVTable* rewamp_ntgs_plugin(void);
#endif
long long rewamp_waveform_pos(void) { return 0; }
int rewamp_waveform_read_i8(int a, signed char* b, int c) { (void)a; (void)b; (void)c; return 0; }
int rewamp_ext_in_list(const char* a, const char* const* b) { (void)a; (void)b; return 0; }

static void put32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <module> [seconds] [out.wav]\n", argv[0]); return 2; }
    const double secs = argc > 2 ? atof(argv[2]) : 20.0;
    const char* wav = argc > 3 ? argv[3] : NULL;
    rewamp_channel_data_init();
#ifdef NTGS_ORACLE
    return oracle_main(argv[1], secs);
#endif

    /* sonde: par le contenu seulement, ext NULL ou fantaisiste */
    {
        FILE* f = fopen(argv[1], "rb");
        uint8_t hdr[64] = {0};
        size_t n = f ? fread(hdr, 1, sizeof hdr, f) : 0;
        if (f) fclose(f);
        int s1 = rewamp_ntgs_plugin()->probe(NULL, hdr, n);
        int s2 = rewamp_ntgs_plugin()->probe(" who meets devo", hdr, n);
        printf("probe=%d/%d\n", s1, s2);
        if (s1 < 90) { fprintf(stderr, "probe refused\n"); return 1; }
    }
    const RewampPluginVTable* vt = rewamp_ntgs_plugin();
    RewampAudioFormat fmt; memset(&fmt, 0, sizeof fmt);
    RewampDecoder* dec = vt->open(argv[1], &fmt);
    if (!dec) { fprintf(stderr, "open failed\n"); return 1; }
    printf("%s", rewamp_track_message());
    const int voices = rewamp_channel_count_raw();
    const uint64_t len = vt->length(dec);
    printf("rate=%u ch=%u voices=%d length=%.2fs\n", fmt.sampleRate, fmt.channels, voices,
           len / (double)fmt.sampleRate);

    FILE* out = NULL;
    if (wav) {
        out = fopen(wav, "wb");
        if (out) { fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, out); put32(out, 16); put16(out, 1); put16(out, 2);
                   put32(out, fmt.sampleRate); put32(out, fmt.sampleRate * 4); put16(out, 4); put16(out, 16);
                   fwrite("data\0\0\0\0", 1, 8, out); }
    }
    enum { CH = 1024 };
    float buf[CH * 2];
    double sum = 0, vsum[64] = {0}; float peak = 0;
    uint64_t done = 0, total = (uint64_t)(secs * fmt.sampleRate), clipped = 0;
    int64_t prev[64]; for (int v = 0; v < 64; v++) prev[v] = m_voice_current_ptr[v];
    /* fenêtre de référence pour vérifier seek(): 1 s à partir de 10 s */
    const uint64_t refAt = 10ull * fmt.sampleRate, refLen = fmt.sampleRate;
    float* ref = (float*)calloc(refLen * 2, sizeof(float));
    uint64_t refGot = 0;
    while (done < total) {
        uint64_t want = total - done < CH ? total - done : CH;
        uint64_t n = vt->read(dec, buf, want);
        if (n == 0) { printf("end of song at %.2fs\n", done / (double)fmt.sampleRate); break; }
        for (uint64_t i = 0; i < n; i++) {
            uint64_t t = done + i;
            if (t >= refAt && t < refAt + refLen) { ref[(t - refAt) * 2] = buf[i * 2]; ref[(t - refAt) * 2 + 1] = buf[i * 2 + 1]; refGot++; }
        }
        for (uint64_t i = 0; i < n * 2; i++) {
            float s = buf[i]; sum += (double)s * s;
            if (fabsf(s) > peak) peak = fabsf(s);
            if (fabsf(s) >= 1.0f) clipped++;
            if (out) { int v = (int)lrintf(s * 32767.f); put16(out, (uint16_t)(int16_t)v); }
        }
        for (int v = 0; v < voices && v < 64; v++) {
            int64_t p = m_voice_current_ptr[v] >> MODIZER_OSCILLO_OFFSET_FIXEDPOINT;
            int64_t q = prev[v] >> MODIZER_OSCILLO_OFFSET_FIXEDPOINT;
            for (int64_t k = q; k < p; k++) { int s = m_voice_buff[v][k & 4095]; vsum[v] += s * s; }
            prev[v] = m_voice_current_ptr[v];
        }
        done += n;
    }
    if (out) {
        uint32_t data = (uint32_t)(done * 4);
        fseek(out, 4, SEEK_SET); put32(out, 36 + data);
        fseek(out, 40, SEEK_SET); put32(out, data);
        fclose(out);
    }
    const double rms = done ? sqrt(sum / (double)(done * 2)) : 0;
    printf("rendered=%.2fs rms=%.4f (%.1f dBFS) peak=%.3f clipped=%llu\n", done / (double)fmt.sampleRate,
           rms, rms > 0 ? 20 * log10(rms) : -999.0, peak, (unsigned long long)clipped);
    printf("voice rms:");
    for (int v = 0; v < voices && v < 64; v++) printf(" %s=%.1f", modizVoicesName[v], done ? sqrt(vsum[v] / done) : 0);
    printf("\n");

    /* seek(10 s) doit redonner exactement la même fenêtre que la lecture continue */
    if (refGot == refLen) {
        vt->seek(dec, refAt);
        uint64_t got = 0; double maxd = 0;
        while (got < refLen) {
            uint64_t want = refLen - got < CH ? refLen - got : CH;
            uint64_t n = vt->read(dec, buf, want);
            if (!n) break;
            for (uint64_t i = 0; i < n * 2; i++) { double e = fabs(buf[i] - ref[got * 2 + i]); if (e > maxd) maxd = e; }
            got += n;
        }
        printf("seek check: max |diff|=%.6f over %llu frames\n", maxd, (unsigned long long)got);
    }
    free(ref);
    /* seek + mute: ne doit pas planter, et un mute total doit donner le silence */
    vt->seek(dec, len / 2);
    generic_mute_mask = -1;
    double msum = 0;
    for (int i = 0; i < 20; i++) { uint64_t n = vt->read(dec, buf, CH); for (uint64_t k = 0; k < n * 2; k++) msum += buf[k] * buf[k]; }
    generic_mute_mask = 0;
    printf("muted after seek: rms=%.6f\n", sqrt(msum / (20.0 * CH * 2)));
    vt->close(dec);
    return rms > 1e-4 ? 0 : 3;
}
