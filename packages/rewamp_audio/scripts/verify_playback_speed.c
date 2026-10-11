/* Oracle de la vitesse NATIVE d'un greffon (vt->set_speed), sur le moteur
 * compilé, chargé par dlopen — le greffon que le registre choisit vraiment.
 *
 * Contrat vérifié (rewamp_plugin.h, set_speed): sous vitesse native, la SORTIE
 * reste au rythme réel mais le MORCEAU avance s fois plus vite, et length() /
 * seek() restent en temps du morceau à 1×. Donc à 2×:
 *   - le morceau entier se rend en length/2 trames;
 *   - après seek(length/2), le reste se rend en length/4 trames.
 * Un greffon qui refuse (rend 0) relève du rééchantillonnage générique, testé
 * par verify_gapless_handoff.sh.
 *
 *   verify_playback_speed <framework> <data_dir> <vitesse> fichier...      */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rewamp_plugin.h"
typedef void (*fv)(void);
typedef void (*fsd)(const char*);
typedef int (*frank)(const char*, const RewampPluginVTable**, int);

static uint64_t drain(const RewampPluginVTable* vt, RewampDecoder* d, uint64_t cap) {
    static float buf[4096 * 8];
    uint64_t n = 0;
    while (n < cap) {
        uint64_t r = vt->read(d, buf, 4096);
        if (!r) break;
        n += r;
    }
    return n;
}

/* ── Mode NOTES (engines sans fin: SID, NSF…) ──────────────────────────
 * Suite des notes publiées par voie (vgm_last_note, Hz) sur `songSecs` de
 * MORCEAU, en partant de `fromSong` (seek). Un événement = (instant en temps du
 * morceau, voie, Hz) quand la note d'une voie change. */
typedef struct { double t; int v; unsigned hz; } Ev;
static int note_trace(const RewampPluginVTable* vt, const char* path, double speed,
                      double fromSong, double songSecs, unsigned* notes,
                      Ev* ev, int evMax) {
    RewampAudioFormat f = {0};
    RewampDecoder* d = vt->open(path, &f);
    if (!d) return -1;
    if (fabs(speed - 1.0) > 1e-6 && (!vt->set_speed || !vt->set_speed(d, speed))) {
        vt->close(d); return -2;
    }
    const double rate = f.sampleRate ? f.sampleRate : 44100;
    if (fromSong > 0) vt->seek(d, (uint64_t)(fromSong * rate));
    /* Relevé à pas CONSTANT en temps du morceau (256 trames de morceau): à
     * 2× un bloc de sortie couvre deux fois plus de morceau, et un relevé par
     * bloc de sortie sous-échantillonnerait les arpèges rapides. */
    static float buf[2048 * 8];
    const uint64_t blk = (uint64_t)llround(256.0 / speed) ? (uint64_t)llround(256.0 / speed) : 1;
    unsigned last[8] = {0}, cand[8] = {0};
    double candT[8] = {0};
    int n = 0;
    double out = 0;
    while ((out * speed) / rate < songSecs) {
        uint64_t r = vt->read(d, buf, blk);
        if (!r) break;
        out += (double)r;
        const double t = fromSong + out * speed / rate;
        /* Seule une HAUTEUR qui change compte: les silences et la tenue d'une
         * note après relâchement (comptée par certains greffons en trames de
         * sortie) ne disent rien du tempo. */
        /* …et seulement une hauteur STABLE: vue à deux relevés consécutifs
         * (≥ 5,8 ms de morceau). Une routine de jeu écrit l'octet bas puis
         * l'octet haut d'une fréquence: un relevé tombé entre les deux voit
         * une fréquence FAUSSE, qui ne se reproduit pas à une autre vitesse. */
        for (int v = 0; v < 8; v++) {
            unsigned hz = notes[v];
            if (hz && hz != last[v]) {
                if (hz == cand[v]) {
                    last[v] = hz;
                    if (n < evMax) { ev[n].t = candT[v]; ev[n].v = v; ev[n].hz = hz; n++; }
                } else {
                    cand[v] = hz; candT[v] = t;
                }
            } else {
                cand[v] = 0;
            }
        }
    }
    vt->close(d);
    return n;
}

/* Part des notes de `a` retrouvées dans `b`: même voie, même hauteur (à
 * 0,2 % près), à `tol` près en temps du MORCEAU. Par ENSEMBLE et non en
 * séquence: le greffon relit les registres à des instants qui dépendent de la
 * vitesse et attrape parfois un état fugace en plus (un registre écrit entre
 * deux relevés) — un seul événement en trop désalignait une comparaison
 * séquentielle. Rend le rappel; *dtMax = pire écart parmi les appariés. */
static double compare_at(const Ev* a, int na, const Ev* b, int nb, double tol, double off, double* dtMax) {
    int match = 0, j0 = 0;
    *dtMax = 0;
    for (int i = 0; i < na; i++) {
        const double ta = a[i].t + off;
        while (j0 < nb && b[j0].t < ta - tol) j0++;
        double best = 1e9;
        for (int j = j0; j < nb && b[j].t <= ta + tol; j++) {
            if (b[j].v != a[i].v) continue;
            if (fabs((double)a[i].hz - b[j].hz) > 1.0 + 0.002 * a[i].hz) continue;
            const double dt = fabs(ta - b[j].t);
            if (dt < best) best = dt;
        }
        if (best <= tol) { match++; if (best > *dtMax) *dtMax = best; }
    }
    return na ? (double)match / na : 0.0;
}

/* Meilleur décalage CONSTANT (±3 s, pas de 10 ms) avant d'apparier: un
 * pilote qui attend au démarrage en comptant des cycles CPU (KSS) prend le
 * même temps de SORTIE à toute vitesse, donc un temps de MORCEAU différent —
 * un simple retard de départ, sans effet sur le tempo. */
static double g_lastOffset = 0;
static double compare(const Ev* a, int na, const Ev* b, int nb, double tol, double* dtMax) {
    double bestR = -1, bestOff = 0, bestDt = 0;
    for (int k = -300; k <= 300; k++) {
        double dt;
        const double r = compare_at(a, na, b, nb, tol, k * 0.01, &dt);
        if (r > bestR + 1e-9 || (fabs(r - bestR) < 1e-9 && abs(k) < fabs(bestOff) * 100)) {
            bestR = r; bestOff = k * 0.01; bestDt = dt;
        }
    }
    *dtMax = bestDt;
    g_lastOffset = bestOff;
    return bestR;
}

/* ── Mode SPECTRE (engines qui ne publient pas de notes) ───────────────
 * Rend `songSecs` de MORCEAU en mono; à chaque instant t (pas de 100 ms de
 * morceau) compare le spectre log-amplitude (fenêtre de Hann, 2048 points,
 * 100 Hz-8 kHz) du rendu 1× et du rendu à la vitesse testée. Hauteur
 * conservée + tempo × vitesse ⇒ les spectres coïncident; un rééchantillonnage
 * (hauteur × vitesse) les décale. Rend la similarité cosinus moyenne. */
static float* render_mono(const RewampPluginVTable* vt, const char* path, double speed,
                          double songSecs, int forceGeneric, uint64_t* outN, double* rateOut) {
    RewampAudioFormat f = {0};
    RewampDecoder* d = vt->open(path, &f);
    if (!d) return NULL;
    if (fabs(speed - 1.0) > 1e-6 && !forceGeneric && (!vt->set_speed || !vt->set_speed(d, speed))) {
        vt->close(d); return NULL;
    }
    const double rate = f.sampleRate ? f.sampleRate : 44100;
    const unsigned ch = f.channels ? f.channels : 2;
    /* « générique » simulé: rendre à 1× puis lire × vitesse (bande). */
    const double rspeed = forceGeneric ? 1.0 : speed;
    const uint64_t want = (uint64_t)(songSecs * rate / rspeed) + 4096;
    float* mono = (float*)calloc(want, sizeof(float));
    static float buf[1024 * 8];
    uint64_t n = 0;
    while (n < want) {
        uint64_t r = vt->read(d, buf, 1024);
        if (!r) break;
        for (uint64_t i = 0; i < r && n < want; i++, n++) {
            float acc = 0; for (unsigned c = 0; c < ch; c++) acc += buf[i * ch + c];
            mono[n] = acc / ch;
        }
    }
    vt->close(d);
    if (forceGeneric && fabs(speed - 1.0) > 1e-6) {
        /* bande: la sortie lit l'entrée × vitesse (interpolation linéaire) */
        const uint64_t m = (uint64_t)(n / speed);
        float* g = (float*)calloc(m + 1, sizeof(float));
        for (uint64_t i = 0; i < m; i++) {
            double x = i * speed; uint64_t k = (uint64_t)x; double fr = x - k;
            g[i] = (float)(mono[k] * (1 - fr) + (k + 1 < n ? mono[k + 1] : 0) * fr);
        }
        free(mono); mono = g; n = m;
    }
    *outN = n; *rateOut = rate;
    return mono;
}
static void fft(double* re, double* im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double a = -2 * M_PI / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                const double wr = cos(a * k), wi = sin(a * k);
                const double ur = re[i + k], ui = im[i + k];
                const double vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                const double vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
            }
    }
}
#define SPN 2048
static int spectrum(const float* x, uint64_t n, uint64_t at, double rate, double* out) {
    if (at + SPN > n) return 0;
    static double re[SPN], im[SPN];
    for (int i = 0; i < SPN; i++) {
        const double w = 0.5 - 0.5 * cos(2 * M_PI * i / (SPN - 1));
        re[i] = x[at + i] * w; im[i] = 0;
    }
    fft(re, im, SPN);
    const int lo = (int)(100.0 * SPN / rate), hi = (int)(8000.0 * SPN / rate);
    for (int k = lo; k < hi; k++) out[k - lo] = log10(1e-6 + sqrt(re[k] * re[k] + im[k] * im[k]));
    return hi - lo;
}
static double spec_compare(const float* a, uint64_t na, double ra, const float* b, uint64_t nb,
                           double rb, double sp, double songSecs) {
    static double sa[SPN], sb[SPN];
    double sum = 0; int cnt = 0;
    for (double t = 1.0; t < songSecs - 0.5; t += 0.1) {
        const int ka = spectrum(a, na, (uint64_t)(t * ra), ra, sa);
        const int kb = spectrum(b, nb, (uint64_t)(t * rb / sp), rb, sb);
        if (!ka || ka != kb) continue;
        double ma = 0, mb = 0;
        for (int k = 0; k < ka; k++) { ma += sa[k]; mb += sb[k]; }
        ma /= ka; mb /= ka;
        double dot = 0, ea = 0, eb = 0;
        for (int k = 0; k < ka; k++) {
            const double x = sa[k] - ma, y = sb[k] - mb;
            dot += x * y; ea += x * x; eb += y * y;
        }
        if (ea > 0 && eb > 0) { sum += dot / sqrt(ea * eb); cnt++; }
    }
    return cnt ? sum / cnt : 0;
}

/* TEMPO: corrélation de Pearson des enveloppes RMS (pas de 20 ms de MORCEAU). */
static double env_corr(const float* a, uint64_t na, double ra, const float* b, uint64_t nb,
                       double rb, double sp, double songSecs) {
    const int N = (int)((songSecs - 0.5) / 0.02);
    double* ea = (double*)calloc(N, sizeof(double));
    double* eb = (double*)calloc(N, sizeof(double));
    int n = 0;
    for (int k = 0; k < N; k++) {
        const double t = 0.25 + k * 0.02;
        const uint64_t a0 = (uint64_t)(t * ra), a1 = (uint64_t)((t + 0.02) * ra);
        const uint64_t b0 = (uint64_t)(t * rb / sp), b1 = (uint64_t)((t + 0.02) * rb / sp);
        if (a1 > na || b1 > nb || b1 <= b0) break;
        double sa = 0, sb = 0;
        for (uint64_t i = a0; i < a1; i++) sa += a[i] * a[i];
        for (uint64_t i = b0; i < b1; i++) sb += b[i] * b[i];
        ea[n] = sqrt(sa / (a1 - a0)); eb[n] = sqrt(sb / (b1 - b0)); n++;
    }
    double ma = 0, mb = 0;
    for (int k = 0; k < n; k++) { ma += ea[k]; mb += eb[k]; }
    if (n) { ma /= n; mb /= n; }
    double dot = 0, va = 0, vb = 0;
    for (int k = 0; k < n; k++) {
        dot += (ea[k] - ma) * (eb[k] - mb); va += (ea[k] - ma) * (ea[k] - ma); vb += (eb[k] - mb) * (eb[k] - mb);
    }
    free(ea); free(eb);
    return (va > 0 && vb > 0) ? dot / sqrt(va * vb) : 0;
}
/* HAUTEUR: spectre log MOYEN sur la fenêtre (1× contre vitesse testée). */
static double mean_spec(const float* a, uint64_t na, double ra, const float* b, uint64_t nb,
                        double rb, double sp, double songSecs) {
    static double sa[SPN], sb[SPN], acc_a[SPN], acc_b[SPN];
    memset(acc_a, 0, sizeof acc_a); memset(acc_b, 0, sizeof acc_b);
    int K = 0;
    for (double t = 0.5; t < songSecs - 0.5; t += 0.05) {
        const int ka = spectrum(a, na, (uint64_t)(t * ra), ra, sa);
        const int kb = spectrum(b, nb, (uint64_t)(t * rb / sp), rb, sb);
        if (!ka || ka != kb) continue;
        for (int k = 0; k < ka; k++) { acc_a[k] += sa[k]; acc_b[k] += sb[k]; }
        K = ka;
    }
    double ma = 0, mb = 0;
    for (int k = 0; k < K; k++) { ma += acc_a[k]; mb += acc_b[k]; }
    if (K) { ma /= K; mb /= K; }
    double dot = 0, ea = 0, eb = 0;
    for (int k = 0; k < K; k++) {
        const double x = acc_a[k] - ma, y = acc_b[k] - mb;
        dot += x * y; ea += x * x; eb += y * y;
    }
    return (ea > 0 && eb > 0) ? dot / sqrt(ea * eb) : 0;
}

int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s framework data_dir speed files...\n", argv[0]); return 2; }
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    ((fsd)dlsym(h, "rewamp_set_data_dir"))(argv[2]);
    ((fv)dlsym(h, "rewamp_register_builtin_plugins"))();
    frank rank = (frank)dlsym(h, "rewamp_registry_select_ranked");
    /* PLUGIN=nom — n'essaie que ce greffon (ex. PLUGIN=fluidlite quand les
     * ROMs MT-32 sont là et prennent la main sur les .mid). */
    const char* only = getenv("PLUGIN");
    /* SF2=chemin — SoundFont de FluidLite (l'app la pose par réglage). */
    if (getenv("SF2") != NULL)
        ((void (*)(const char*))dlsym(h, "rewamp_midi_set_soundfont"))(getenv("SF2"));
    const double sp = atof(argv[3]);
    int fails = 0;
    for (int i = 4; i < argc; i++) {
        const RewampPluginVTable* c[8];
        int n = rank(argv[i], c, 8);
        const RewampPluginVTable* vt = NULL;
        RewampDecoder* d = NULL;
        RewampAudioFormat f = {0};
        for (int k = 0; k < n && !d; k++) {
            if (only != NULL && strcmp(c[k]->name, only) != 0) continue;
            d = c[k]->open(argv[i], &f); if (d) vt = c[k];
        }
        const char* base = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        if (!d) { printf("%-40s OUVERTURE ECHOUEE\n", base); fails++; continue; }
        const uint64_t len = vt->length ? vt->length(d) : 0;
        if (!vt->set_speed) { printf("%-40s %-14s pas de set_speed (generique)\n", base, vt->name); vt->close(d); continue; }
        if (getenv("SPEC") != NULL) {
            vt->close(d);
            const double W = 12.0;
            uint64_t n1, n2, n3; double r1, r2, r3;
            float* a = render_mono(vt, argv[i], 1.0, W, 0, &n1, &r1);
            float* b = render_mono(vt, argv[i], sp, W, 0, &n2, &r2);
            float* g = render_mono(vt, argv[i], sp, W, 1, &n3, &r3);
            if (!a || !b || !g) { printf("%-40s %-14s set_speed refuse -> generique\n", base, vt->name); free(a); free(b); free(g); continue; }
            const double tn = env_corr(a, n1, r1, b, n2, r2, sp, W);   /* tempo, natif */
            const double tg = env_corr(a, n1, r1, g, n3, r3, sp, W);   /* tempo, bande */
            const double pn = mean_spec(a, n1, r1, b, n2, r2, sp, W);  /* hauteur, natif */
            const double pg = mean_spec(a, n1, r1, g, n3, r3, sp, W);  /* hauteur, bande */
            /* La bande suit le tempo à la perfection (c'est le même son lu plus
             * vite): son enveloppe sert d'étalon. Le natif doit suivre le
             * tempo presque autant, ET garder le spectre que la bande perd. */
            const int good = tn > 0.6 && pn > 0.9 && pn > pg + 0.05;
            if (!good) fails++;
            printf("%-40s %-14s %s  tempo=%.2f (bande %.2f)  hauteur=%.3f (bande %.3f)\n",
                   base, vt->name, good ? "OK   " : "ECHEC", tn, tg, pn, pg);
            free(a); free(b); free(g);
            continue;
        }
        if (getenv("NOTES") != NULL) {
            vt->close(d);
            unsigned* notes = (unsigned*)dlsym(h, "vgm_last_note");
            static Ev e1[20000], e2[20000], e3[20000], e4[20000];
            const double W = 20.0;
            int n1 = note_trace(vt, argv[i], 1.0, 0, W, notes, e1, 20000);
            int n2 = note_trace(vt, argv[i], sp,  0, W, notes, e2, 20000);
            int n3 = note_trace(vt, argv[i], 1.0, 10, W - 10, notes, e3, 20000);
            int n4 = note_trace(vt, argv[i], sp,  10, W - 10, notes, e4, 20000);
            if (n2 == -2) { printf("%-40s %-14s set_speed refuse -> generique\n", base, vt->name); continue; }
            double dt1, dt2;
            /* tolérance: un bloc de lecture (512 trames) × vitesse, plus la
             * quantification de la routine de jeu (~1 trame de 50/60 Hz) */
            const double tol = 0.040;   /* ~2 trames de la routine de jeu (50/60 Hz) */
            if (getenv("DUMP") != NULL) {
                for (int k = 0; k < 40 && (k < n1 || k < n2); k++)
                    printf("   %8.3f v%d %5u   |   %8.3f v%d %5u\n",
                           k < n1 ? e1[k].t : -1, k < n1 ? e1[k].v : -1, k < n1 ? e1[k].hz : 0,
                           k < n2 ? e2[k].t : -1, k < n2 ? e2[k].v : -1, k < n2 ? e2[k].hz : 0);
            }
            const double r1 = compare(e1, n1, e2, n2, tol, &dt1);
            const double off1 = g_lastOffset;
            const double r2 = compare(e3, n3, e4, n4, tol, &dt2);
            const int good = n1 > 10 && r1 > 0.9 && r2 > 0.85;
            if (!good) fails++;
            printf("%-40s %-14s %s  notes 1x=%d %gx=%d apparies=%.0f%% (ecart max %.3fs, decalage %+.2fs)  apres seek 10s: %.0f%% (%.3fs)\n",
                   base, vt->name, good ? "OK   " : "ECHEC", n1, sp, n2, r1 * 100, dt1, off1, r2 * 100, dt2);
            continue;
        }
        const int ok = vt->set_speed(d, sp);
        if (!ok || len == 0) {
            printf("%-40s %-14s set_speed=%d len=%llu -> generique\n", base, vt->name, ok, (unsigned long long)len);
            vt->close(d); continue;
        }
        const uint64_t cap = (uint64_t)((double)len / sp * 1.5) + 44100;
        const uint64_t full = drain(vt, d, cap);
        vt->close(d);
        d = vt->open(argv[i], &f);
        vt->set_speed(d, sp);
        vt->seek(d, len / 2);
        const uint64_t half = drain(vt, d, cap);
        vt->close(d);
        const double e1 = fabs((double)full - (double)len / sp) / ((double)len / sp);
        const double e2 = fabs((double)half - (double)len / 2 / sp) / ((double)len / 2 / sp);
        const int good = e1 < 0.02 && e2 < 0.03;
        if (!good) fails++;
        printf("%-40s %-14s %s  len=%.2fs  entier=%.2fs (att. %.2f)  apres seek=%.2fs (att. %.2f)\n",
               base, vt->name, good ? "OK   " : "ECHEC", len / (double)f.sampleRate,
               full / (double)f.sampleRate, len / sp / f.sampleRate,
               half / (double)f.sampleRate, len / 2 / sp / f.sampleRate);
    }
    return fails != 0;
}
