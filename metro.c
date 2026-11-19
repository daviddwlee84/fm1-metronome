/* SPDX-License-Identifier: GPL-3.0-or-later */
/* METRONOME's core: see metro.h */
#include "metro.h"

static const uint8_t DIVS[] = { 1, 2, 3, 4 };

int mt_div_max(int bpb)
{
    int i, best = 1;
    for (i = 0; i < 4; i++)
        if (bpb * DIVS[i] <= (int)MT_STEPS) best = DIVS[i];
    return best;
}

uint32_t mt_nsteps(const mt_slot_t *s) { return (uint32_t)s->bpb * s->div; }

void mt_grid_default(mt_slot_t *s)
{
    uint32_t i, n = mt_nsteps(s);
    for (i = 0; i < MT_STEPS; i++)
        s->steps[i] = (uint8_t)(i >= n ? MT_OFF : i == 0 ? MT_ACCENT : i % s->div == 0 ? MT_NORMAL : MT_GHOST);
}

void mt_slot_default(mt_slot_t *s)
{
    uint32_t i;
    s->used = 0;
    s->mode = MT_GRID;
    s->bpm = 120;
    s->bpb = 4;
    s->div = 1;
    s->beat = 4;
    s->sound = MT_SND_CLICK;
    for (i = 0; i < MT_NLAYER; i++) s->mix[i] = 0;
    s->mix[MT_L4] = MT_VOL_MAX;
    mt_grid_default(s);
}

int mt_slot_valid(const mt_slot_t *s)
{
    uint32_t i;
    int ok = s->used <= 1 && s->mode < MT_NMODE && s->bpm >= MT_BPM_MIN && s->bpm <= MT_BPM_MAX && s->bpb >= 1
             && s->bpb <= 9 && s->div >= 1 && s->div <= 4 && s->bpb * s->div <= (int)MT_STEPS
             && s->beat <= MT_BEAT_MAX && s->sound < MT_NSND;
    for (i = 0; ok && i < MT_NLAYER; i++) ok = s->mix[i] <= MT_VOL_MAX;
    for (i = 0; ok && i < MT_STEPS; i++) ok = s->steps[i] <= MT_ACCENT;
    return ok;
}

uint32_t mt_bar_ticks(const mt_slot_t *s)
{
    if (s->mode == MT_GRID) return (uint32_t)s->bpb * MT_TPB;
    return (uint32_t)(s->beat ? s->beat : 1) * MT_TPB;
}

int32_t mt_step_at(const mt_slot_t *s, uint32_t tick)
{
    uint32_t per = MT_TPB / s->div;
    return (int32_t)((tick / per) % mt_nsteps(s));
}

static void add(mt_hits_t *h, int v, int vol)
{
    if (vol <= 0 || h->n >= MT_NLAYER) return;
    h->v[h->n] = (uint8_t)v;
    h->vol[h->n] = (uint8_t)vol;
    h->n++;
}

void mt_hits(const mt_slot_t *s, uint32_t tick, mt_hits_t *h)
{
    uint32_t in = tick % MT_TPB, beat = tick / MT_TPB;
    h->n = 0;
    if (s->mode == MT_GRID) {
        uint32_t per = MT_TPB / s->div;
        int lvl;
        if (in % per) return;
        lvl = s->steps[mt_step_at(s, tick)];
        if (lvl == MT_ACCENT) add(h, MT_V_ACCENT, MT_VOL_MAX);
        else if (lvl == MT_NORMAL) add(h, MT_V_NORMAL, MT_VOL_MAX);
        else if (lvl == MT_GHOST) add(h, MT_V_GHOST, MT_VOL_MAX);
        return;
    }
    /* CLASSIC: the quarter on the beat (accented every BEAT beats), the other layers between the beats */
    if (!in) add(h, s->beat && beat % s->beat == 0 ? MT_V_ACCENT : MT_V_NORMAL, s->mix[MT_L4]);
    else {
        if (in == 6) add(h, MT_V_8, s->mix[MT_L8]);
        if (in % 4 == 0) add(h, MT_V_3, s->mix[MT_L3]);
        if (in % 3 == 0) add(h, MT_V_16, s->mix[MT_L16]);
    }
}

uint32_t mt_tick_q8(uint32_t bpm, uint32_t sr) { return sr * 60u * 256u / bpm / MT_TPB; }   /* 44100: fits 32 bits */

uint32_t mt_tap_bpm(const uint32_t *ms, uint32_t n)
{
    uint32_t span, bpm;
    if (n < 2) return 0;
    span = ms[n - 1] - ms[0];
    if (!span) return 0;
    bpm = (60000u * (n - 1) + span / 2) / span;
    return bpm < MT_BPM_MIN ? MT_BPM_MIN : bpm > MT_BPM_MAX ? MT_BPM_MAX : bpm;
}
