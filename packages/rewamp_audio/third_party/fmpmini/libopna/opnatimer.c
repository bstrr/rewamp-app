#include "opnatimer.h"
#include "opna.h"
#include "oscillo/oscillo.h"
#include <math.h>

enum {
  TIMERA_BITS = 10,
  TIMERB_SHIFT = 4,
  TIMERB_BITS = 8 + TIMERB_SHIFT,
};

/* rewamp: native playback speed. The timers count in their own units (one
 * per output sample upstream); at speed s they advance s units per rendered
 * sample, so the driver they call runs s times faster while the chip keeps
 * its pitch. A step stops EXACTLY on the next expiry (the excess goes back
 * into the fractional remainder). 1.0 = upstream, bit for bit. */
double opna_timer_rewamp_speed = 1.0;
static double s_rewamp_frac = 0.0;

static void rewamp_timer_step(const struct opna_timer *timer, unsigned samples,
                              unsigned *gen, unsigned *adv) {
  unsigned need = 0xffffffffu;
  if (timer->timerb_enable && timer->timerb_load) {
    unsigned n = (1<<TIMERB_BITS) - timer->timerb_cnt;
    if (n < need) need = n;
  }
  if (timer->timera_enable && timer->timera_load) {
    unsigned n = (1<<TIMERA_BITS) - timer->timera;
    if (n < need) need = n;
  }
  const double sp = opna_timer_rewamp_speed;
  if (sp == 1.0) {
    *gen = *adv = (need < samples) ? need : samples;
    return;
  }
  unsigned g = samples;
  if (need != 0xffffffffu) {
    double want = ((double)need - s_rewamp_frac) / sp;
    unsigned gn = want <= 1.0 ? 1u : (unsigned)ceil(want);
    if (gn < g) g = gn;
  }
  double units = s_rewamp_frac + (double)g * sp;
  unsigned a = (unsigned)units;
  if (a > need) a = need;
  s_rewamp_frac = units - (double)a;
  *gen = g;
  *adv = a;
}

void opna_timer_reset(struct opna_timer *timer, struct opna *opna) {
  s_rewamp_frac = 0.0;
  timer->opna = opna;
  timer->status = 0;
  timer->interrupt_cb = 0;
  timer->interrupt_userptr = 0;
  timer->mix_cb = 0;
  timer->mix_userptr = 0;
  timer->timerb = 0;
  timer->timerb_load = false;
  timer->timerb_enable = false;
  timer->timerb_cnt = 0;
}

uint8_t opna_timer_status(const struct opna_timer *timer) {
  return timer->status;
}

void opna_timer_set_int_callback(struct opna_timer *timer, opna_timer_int_cb_t func, void *userptr) {
  timer->interrupt_cb = func;
  timer->interrupt_userptr = userptr;
}

void opna_timer_set_mix_callback(struct opna_timer *timer, opna_timer_mix_cb_t func, void *userptr) {
  timer->mix_cb = func;
  timer->mix_userptr = userptr;
}

void opna_timer_writereg(struct opna_timer *timer, unsigned reg, unsigned val) {
  val &= 0xff;
  opna_writereg(timer->opna, reg, val);
  switch (reg) {
  case 0x24:
    timer->timera &= ~0xff;
    timer->timera |= val;
    break;
  case 0x25:
    timer->timera &= 0xff;
    timer->timera |= ((val & 3) << 8);
    break;
  case 0x26:
    timer->timerb = val;
    timer->timerb_cnt = timer->timerb << TIMERB_SHIFT;
    break;
  case 0x27:
    timer->timera_load = val & (1<<0);
    timer->timera_enable = val & (1<<2);
    timer->timerb_load = val & (1<<1);
    timer->timerb_enable = val & (1<<3);
    
    if (val & (1<<4)) {
      timer->status &= ~(1<<0);
    }
    if (val & (1<<5)) {
      //timer->timerb_cnt = timer->timerb << TIMERB_SHIFT;
      timer->status &= ~(1<<1);
    }
  }
}

void opna_timer_mix(struct opna_timer *timer, int16_t *buf, unsigned samples) {
  opna_timer_mix_oscillo(timer, buf, samples, 0);
}

void opna_timer_mix_oscillo(struct opna_timer *timer, int16_t *buf, unsigned samples, struct oscillodata *oscillo) {
  do {
    unsigned generate_samples, advance;
    rewamp_timer_step(timer, samples, &generate_samples, &advance);
    if (buf) opna_mix_oscillo(timer->opna, buf, generate_samples, oscillo);
    if (timer->mix_cb) {
      if (buf) timer->mix_cb(timer->mix_userptr, buf, generate_samples);
    }
    if (buf) buf += generate_samples*2;
    samples -= generate_samples;
    if (timer->timera_load) {
      timer->timera = (timer->timera + advance) & ((1<<TIMERA_BITS)-1);
      if (!timer->timera && timer->timera_enable) {
        if (!(timer->status & (1<<0))) {
          timer->status |= (1<<0);
          timer->interrupt_cb(timer->interrupt_userptr);
        }
      }
      timer->timera &= (1<<TIMERA_BITS)-1;
    }
    if (timer->timerb_load) {
      timer->timerb_cnt = (timer->timerb_cnt + advance) & ((1<<TIMERB_BITS)-1);
      if (!timer->timerb_cnt && timer->timerb_enable) {
        if (!(timer->status & (1<<1))) {
          timer->status |= (1<<1);
          timer->interrupt_cb(timer->interrupt_userptr);
        }
      }
    }
  } while (samples);
}

//YOYOFR
void opna_timer_nomix(struct opna_timer *timer, unsigned samples) {
  do {
    unsigned generate_samples, advance;
    rewamp_timer_step(timer, samples, &generate_samples, &advance);
    //opna_mix_oscillo(timer->opna, buf, generate_samples, oscillo);
    if (timer->mix_cb) {
        timer->mix_cb(timer->mix_userptr, NULL, generate_samples);
    }
    samples -= generate_samples;
    if (timer->timera_load) {
      timer->timera = (timer->timera + advance) & ((1<<TIMERA_BITS)-1);
      if (!timer->timera && timer->timera_enable) {
        if (!(timer->status & (1<<0))) {
          timer->status |= (1<<0);
          timer->interrupt_cb(timer->interrupt_userptr);
        }
      }
      timer->timera &= (1<<TIMERA_BITS)-1;
    }
    if (timer->timerb_load) {
      timer->timerb_cnt = (timer->timerb_cnt + advance) & ((1<<TIMERB_BITS)-1);
      if (!timer->timerb_cnt && timer->timerb_enable) {
        if (!(timer->status & (1<<1))) {
          timer->status |= (1<<1);
          timer->interrupt_cb(timer->interrupt_userptr);
        }
      }
    }
  } while (samples);
}
//YOYOFR

