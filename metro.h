/* SPDX-License-Identifier: GPL-3.0-or-later */
/* METRONOME's platform-free core: a preset (slot), what sounds on each tick, tap tempo.
 * A beat is MT_TPB ticks (12: quarter, eighth, triplet and sixteenth all land on a tick). No floats, no platform. */
#pragma once
#include <stdint.h>

#define MT_TPB 12u                       /* ticks a beat */
#define MT_NSLOT 16u                     /* one a white key */
#define MT_STEPS 16u                     /* GRID: steps a bar at most (bpb x div), one a white key */
#define MT_BPM_MIN 30
#define MT_BPM_MAX 300
#define MT_VOL_MAX 7                     /* CLASSIC: a layer's volume 0..7 */
#define MT_BEAT_MAX 9                    /* CLASSIC: the accent every BEAT beats, 0 none */

enum { MT_GRID, MT_CLASSIC, MT_NMODE };
enum { MT_OFF, MT_GHOST, MT_NORMAL, MT_ACCENT };   /* a GRID step's level */
enum { MT_L4, MT_L8, MT_L3, MT_L16, MT_NLAYER };  /* CLASSIC's layers: quarter, eighth, triplet, sixteenth */
enum { MT_SND_CLICK, MT_SND_WOOD, MT_SND_BEEP, MT_NSND };

/* what sounds: GRID's levels and CLASSIC's layers as one voice set */
enum { MT_V_ACCENT, MT_V_NORMAL, MT_V_GHOST, MT_V_8, MT_V_3, MT_V_16, MT_NV };

typedef struct {
    uint8_t used;                        /* stored by the player (its key glows) */
    uint8_t mode;                        /* MT_GRID, MT_CLASSIC */
    uint16_t bpm;                        /* MT_BPM_MIN..MT_BPM_MAX */
    uint8_t bpb;                         /* GRID: beats a bar 1..9 */
    uint8_t div;                         /* GRID: steps a beat 1, 2, 3 (triplets), 4; bpb x div <= MT_STEPS */
    uint8_t beat;                        /* CLASSIC: accent every BEAT beats, 0 none */
    uint8_t sound;                       /* MT_SND_* */
    uint8_t mix[MT_NLAYER];              /* CLASSIC: each layer's volume 0..MT_VOL_MAX */
    uint8_t steps[MT_STEPS];             /* GRID: each step's level MT_OFF..MT_ACCENT */
} mt_slot_t;                             /* 28 bytes */

typedef struct {
    uint8_t n;
    uint8_t v[MT_NLAYER];                /* MT_V_* */
    uint8_t vol[MT_NLAYER];              /* 1..MT_VOL_MAX */
} mt_hits_t;

void mt_slot_default(mt_slot_t *s);      /* 120 BPM, 4/4 quarters, accent on 1, CLASSIC quarters only */
void mt_grid_default(mt_slot_t *s);      /* GRID's pattern for its bpb and div: 1 accented, beats, ghosted steps */
int mt_slot_valid(const mt_slot_t *s);
int mt_div_max(int bpb);                 /* the largest div that fits MT_STEPS */
uint32_t mt_nsteps(const mt_slot_t *s);  /* GRID: steps a bar */
uint32_t mt_bar_ticks(const mt_slot_t *s);   /* a bar (GRID: bpb beats; CLASSIC: BEAT beats, 1 without accent) */
int32_t mt_step_at(const mt_slot_t *s, uint32_t tick);   /* GRID: the step playing at this tick */
void mt_hits(const mt_slot_t *s, uint32_t tick, mt_hits_t *h);   /* what sounds on this tick */
uint32_t mt_tick_q8(uint32_t bpm, uint32_t sr);   /* samples a tick, x256 */
uint32_t mt_tap_bpm(const uint32_t *ms, uint32_t n);   /* the last n (2..4) press times, oldest first: BPM, 0 none */
