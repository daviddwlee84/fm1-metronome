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

static const struct {
    const char *name;
    uint8_t bpb, div;
    const char *steps;                   /* A accent, N normal, G ghost, . off; 0: mt_grid_default */
} PRESET[MT_NPRESET] = {
    { "2/4", 2, 1, 0 },          { "3/4", 3, 1, 0 },          { "4/4", 4, 1, 0 },
    { "5/4", 5, 1, 0 },          { "6/8", 2, 3, 0 },          { "6/8 IN 6", 6, 1, "AGGNGG" },
    { "9/8", 3, 3, 0 },          { "12/8", 4, 3, 0 },         { "5/8 3+2", 5, 1, "AGGNG" },
    { "7/8 2+2+3", 7, 1, "AGNGNGG" },
};

static int pi_ok(int i) { return i < 0 ? 0 : i >= MT_NPRESET ? MT_NPRESET - 1 : i; }

const char *mt_preset_name(int i) { return PRESET[pi_ok(i)].name; }

int mt_preset_eighths(int i) { return PRESET[pi_ok(i)].steps != 0; }

void mt_preset_apply(mt_slot_t *s, int i)
{
    uint32_t k;
    i = pi_ok(i);
    s->bpb = PRESET[i].bpb;
    s->div = PRESET[i].div;
    mt_grid_default(s);
    if (PRESET[i].steps)
        for (k = 0; PRESET[i].steps[k]; k++) {
            char c = PRESET[i].steps[k];
            s->steps[k] = (uint8_t)(c == 'A' ? MT_ACCENT : c == 'N' ? MT_NORMAL : c == 'G' ? MT_GHOST : MT_OFF);
        }
}

int mt_preset_match(const mt_slot_t *s)
{
    int i;
    uint32_t k;
    for (i = 0; i < MT_NPRESET; i++) {
        mt_slot_t t = *s;
        mt_preset_apply(&t, i);
        if (t.bpb != s->bpb || t.div != s->div) continue;
        for (k = 0; k < MT_STEPS && t.steps[k] == s->steps[k]; k++) {}
        if (k == MT_STEPS) return i;
    }
    return -1;
}

static const struct {
    const char *name;
    int16_t lo, typ;                     /* the range starts at lo (the next one's lo ends it); a jump lands on typ */
} MARK[MT_NMARK] = {
    { "LARGHISSIMO", MT_BPM_MIN, 35 }, { "LARGO", 40, 50 },       { "LARGHETTO", 60, 63 },
    { "ADAGIO", 66, 70 },              { "ANDANTE", 76, 92 },     { "MODERATO", 108, 112 },
    { "ALLEGRO", 120, 132 },           { "PRESTO", 168, 184 },    { "PRESTISSIMO", 200, 208 },
};

int mt_marking(int bpm)
{
    int i = MT_NMARK - 1;
    while (i > 0 && bpm < MARK[i].lo) i--;
    return i;
}

const char *mt_marking_name(int i) { return MARK[i < 0 ? 0 : i >= MT_NMARK ? MT_NMARK - 1 : i].name; }

int mt_marking_typical(int i) { return MARK[i < 0 ? 0 : i >= MT_NMARK ? MT_NMARK - 1 : i].typ; }

int mt_marking_jump(int bpm, int dir)
{
    int i = mt_marking(bpm);
    if (dir > 0) return bpm < MARK[i].typ ? MARK[i].typ : i + 1 < MT_NMARK ? MARK[i + 1].typ : bpm;
    return bpm > MARK[i].typ ? MARK[i].typ : i > 0 ? MARK[i - 1].typ : bpm;
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
