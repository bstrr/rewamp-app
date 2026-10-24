/* Oracle: rewamp_load_file hors du fil appelant.
 *   1. un chargement LENT (variante ST2.6 qui fige PTK-Prowiz, ~1 s) sur un fil;
 *   2. pendant ce temps le fil principal martèle play/pause/seek/stop/unload:
 *      chacun doit rendre REWAMP_ERROR_BUSY (-5) — jamais 0, jamais un crash;
 *   3. un second rewamp_load_file concurrent rend BUSY lui aussi;
 *   4. rewamp_probe_subsong_count pendant le chargement ATTEND (durée ≥ reste);
 *   5. après la jointure: load d'un fichier sain = 0, play = 0, position avance.
 * usage: loadbusy <dylib> <datadir> <slow.mod> <good.mod> */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
typedef int (*f_i_v)(void); typedef void (*f_v_s)(const char*); typedef int (*f_i_s)(const char*);
typedef int (*f_i_d)(double); typedef double (*f_d_v)(void); typedef void (*f_v_v)(void);
static f_i_s load; static f_i_v play, pause_, stop, isplaying; static f_i_d seek; static f_v_v unload;
static f_i_s probe; static f_d_v pos;
static double now(void){ struct timeval tv; gettimeofday(&tv,0); return tv.tv_sec+tv.tv_usec/1e6; }
static const char* slow; static volatile int slowDone = 0; static int slowRc = 99; static double slowT = 0;
static void* slow_main(void* a){ (void)a; double t=now(); slowRc = load(slow); slowT = now()-t; slowDone = 1; return 0; }
int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage\n"); return 2; }
    void* h = dlopen(argv[1], RTLD_NOW); if (!h) { puts(dlerror()); return 2; }
#define L(n, t) n = (t)dlsym(h, "rewamp_" #n); if (!n) { printf("no symbol rewamp_%s\n", #n); return 2; }
    f_i_v init; L(init, f_i_v); f_v_s set_data_dir; L(set_data_dir, f_v_s);
    load = (f_i_s)dlsym(h, "rewamp_load_file"); L(play, f_i_v); pause_ = (f_i_v)dlsym(h, "rewamp_pause");
    L(stop, f_i_v); seek = (f_i_d)dlsym(h, "rewamp_seek_seconds"); L(unload, f_v_v); isplaying = (f_i_v)dlsym(h, "rewamp_is_playing");
    probe = (f_i_s)dlsym(h, "rewamp_probe_subsong_count"); pos = (f_d_v)dlsym(h, "rewamp_get_position_seconds");
    set_data_dir(argv[2]);
    { typedef void (*f_pin)(const char*, const char*); f_pin pin = (f_pin)dlsym(h, "rewamp_registry_set_preferred_plugin"); if (!pin) { puts("no pin symbol"); return 2; } pin("mod", "uade"); }
    if (init() != 0) { puts("init failed"); return 2; }
    slow = argv[3];
    pthread_t th; pthread_create(&th, 0, slow_main, 0);
    usleep(50 * 1000);  /* le fil est dedans */
    int busy = 0, other = 0, n = 0;
    while (!slowDone) {
        int r[6] = { play(), pause_(), stop(), seek(1.0), load(argv[4]), isplaying() };
        unload();
        for (int i = 0; i < 5; i++) { n++; if (r[i] == -5) busy++; else other++; }
        if (r[5] != 0) other++;
        usleep(5 * 1000);
    }
    pthread_join(th, 0);
    printf("slow load: rc=%d in %.2fs; commands during load: %d, BUSY=%d, other=%d\n", slowRc, slowT, n, busy, other);
    int fail = other != 0;
    /* 4. sonde pendant un second chargement lent: elle doit attendre */
    slowDone = 0; pthread_create(&th, 0, slow_main, 0); usleep(50 * 1000);
    double t = now(); int cnt = probe(argv[4]); double waited = now() - t;
    pthread_join(th, 0);
    printf("probe during load: count=%d waited %.2fs (slow load took %.2fs)\n", cnt, waited, slowT);
    if (waited < slowT * 0.5) { puts("FAIL: probe did not wait"); fail = 1; }
    /* 5. état sain après */
    int lr = load(argv[4]); int pr = play(); usleep(400 * 1000); double p = pos();
    printf("after: load=%d play=%d playing=%d pos=%.2f\n", lr, pr, isplaying(), p);
    if (lr != 0 || pr != 0) fail = 1;
    stop(); unload();
    puts(fail ? "RESULT: FAIL" : "RESULT: OK");
    return fail;
}
