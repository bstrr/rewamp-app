#ifndef REWAMP_WAVEFORM_H
#define REWAMP_WAVEFORM_H

#include <stdint.h>

/* C linkage for the C++ translation units that read the ring (the projectM
 * renderer does). Without it the declarations mangle and the link fails against
 * rewamp_audio.c - the same trap, mirrored, as the FFI exports. */
#ifdef __cplusplus
extern "C" {
#endif

/* Ring buffer size in stereo frames — must be a power of two. 8192 et non
 * plus 2048: l'oscilloscope stéréo cherche son alignement par corrélation
 * (le même que celui des voies) sur 1700 échantillons EN PLUS des 512
 * affichés, le tout derrière un retard de lecture jusqu'à
 * REWAMP_WAVEFORM_MAX_LAG — 2048 ne couvrait pas les trois. */
#define REWAMP_WAVEFORM_FRAMES 8192

/* Retard maximal de la tête de lecture lissée sur la tête d'écriture
 * (rewamp_get_waveform). Valait FRAMES/2 = 1024: c'est une LATENCE d'affichage,
 * elle ne doit pas suivre la taille de l'anneau. */
#define REWAMP_WAVEFORM_MAX_LAG 1024

/*
 * Called from the audio thread (ds_read) each time the decoder produces PCM.
 * stereo_pcm is interleaved float32 L/R; frame_count is the number of frames.
 * No locking: a torn read during oscilloscope update is visually imperceptible.
 */
void rewamp_waveform_write(const float* pcm, uint64_t frame_count, int channels);

/* Per-voice scope fallback: read last `n` samples of one stereo channel
 * (0=left, 1=right) of the main output as 8-bit signed. Returns count written. */
int rewamp_waveform_read_i8(int channel, int8_t* out, int n);

/* Monotonic write head of the main waveform ring (for scope-fallback updates). */
int64_t rewamp_waveform_pos(void);

#ifdef __cplusplus
}
#endif

#endif /* REWAMP_WAVEFORM_H */
