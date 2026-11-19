/* SPDX-License-Identifier: GPL-3.0-or-later */
/* METRONOME v0.1: SET, GRID and CLASSIC (see app.h). What sounds when is metro.c.
 *
 * Time: the audio's sample count is the clock, as in RHYTHM. The render plays every click on its exact sample (a
 * tick is a twelfth of a beat), not on the next block. A change while playing (another song, a new tempo, a step)
 * is handed to the render as a switch at a tick: the next bar for a song, the next tick for an edit, so the beat
 * goes on without a gap or a jump.
 *
 * Part of fm1-base's unity build, after the platform: gfx.c and announce.c are in scope. */
#include "fastmath.h"
#include "metro.c"
#include "ledfx.c"

#define FW_MAJOR 0
#define FW_MINOR 1
#define SR 44100u

/* ---------------------------------------------------------------- settings --- */
#define REC_MAGIC 0x31304D4Du            /* "MM01" */
#define REC_VER 1u

enum { PG_SET, PG_PAT, PG_N };           /* PAT: the active song's GRID or CLASSIC page (its mode) */
static const char *const SND_NAME[MT_NSND] = { "CLICK", "WOOD", "BEEP" };
static const char *const DIV_NAME[5] = { "", "QUARTERS", "8THS", "TRIPLETS", "16THS" };
static const char *const LAYER_NAME[MT_NLAYER] = { "1/4", "1/8", "TRIP", "1/16" };

typedef struct {
    uint32_t magic;
    uint16_t ver, size;
    uint8_t page;                        /* PG_* */
    uint8_t active;                      /* the song on, 0..MT_NSLOT-1 */
    uint8_t swap_now;                    /* SET: another song starts 0 at the next bar, 1 at once */
    uint8_t rsv;
    mt_slot_t slot[MT_NSLOT];
} rec_t;
static rec_t rec;
static uint32_t saves, dirty_at;
#define CUR (&rec.slot[rec.active])

static void defaults(void)
{
    uint32_t i;
    for (i = 0; i < MT_NSLOT; i++) mt_slot_default(&rec.slot[i]);
    rec.page = PG_SET;
    rec.active = 0;
    rec.swap_now = 0;
    rec.slot[0].used = 1;
}

static int rec_valid(const rec_t *r)
{
    uint32_t i;
    if (r->magic != REC_MAGIC || r->ver != REC_VER || r->size != sizeof *r || r->page >= PG_N
        || r->active >= MT_NSLOT || r->swap_now > 1)
        return 0;
    for (i = 0; i < MT_NSLOT; i++)
        if (!mt_slot_valid(&r->slot[i])) return 0;
    return r->slot[r->active].used;
}

static void rec_save(void)
{
    rec.magic = REC_MAGIC;
    rec.ver = REC_VER;
    rec.size = (uint16_t)sizeof rec;
    if (plat_store_save(OBJ_METRO, &rec, sizeof rec) == 0) saves++;
    dirty_at = 0;
}

/* ------------------------------------------------------- audio and clock --- */
static volatile uint32_t au_blk;         /* the first sample of the block last rendered */
static volatile uint32_t tk_since;       /* ms ticks since that block began */

/* the transport: the main loop writes it under plat_lock, the render plays it */
static struct {
    volatile uint8_t run;
    uint32_t start;                      /* sample of tick 0 */
    uint32_t tick_q8;                    /* samples a tick, x256 */
    uint32_t next, next_at;              /* render: the next tick, its sample */
    mt_slot_t s;                         /* what plays */
    volatile uint8_t pend;               /* a switch waits for tick pend_at's sample */
    uint32_t pend_at, pend_tick, pend_q8;
    mt_slot_t ps;
    volatile uint32_t switches;          /* switches made (checks) */
    volatile uint32_t nv[MT_NV];         /* voices sounded (checks) */
} tr;

typedef struct {
    float ph, inc, amp, dec;
} voice_t;
#define NV 6
static voice_t vo[NV];
static uint32_t vo_rr;

/* q8 steps in 32-bit arithmetic only (no 64-bit division on the device; see RHYTHM) */
static uint32_t q8_mul(uint32_t n, uint32_t q8) { return n * (q8 >> 8) + ((n * (q8 & 255u) + 128u) >> 8); }

static uint32_t tick_at(uint32_t t) { return tr.start + q8_mul(t, tr.tick_q8); }

static void voice(float hz, float amp, float tau_ms)
{
    voice_t *v = &vo[vo_rr++ % NV];
    v->ph = 0.0f;
    v->inc = hz / (float)SR;
    v->amp = amp;
    v->dec = 1.0f - 1000.0f / (tau_ms * (float)SR);   /* e^(-1 / (tau * SR)), near enough */
}

/* the sounds: each voice's pitch (Hz), loudness and decay (ms), per sound set */
static const struct {
    float hz[MT_NV], tau;
} SND[MT_NSND] = {
    { { 1568.0f, 1047.0f, 1047.0f, 784.0f, 880.0f, 659.0f }, 16.0f },    /* CLICK: RHYTHM's */
    { { 1175.0f, 784.0f, 784.0f, 587.0f, 659.0f, 494.0f }, 7.0f },       /* WOOD: lower, shorter */
    { { 2093.0f, 1568.0f, 1568.0f, 1319.0f, 1397.0f, 1175.0f }, 45.0f }, /* BEEP: higher, longer */
};
static const float V_AMP[MT_NV] = { 0.55f, 0.38f, 0.13f, 0.30f, 0.30f, 0.26f };

static void sound(uint32_t t)
{
    mt_hits_t h;
    uint32_t i;
    mt_hits(&tr.s, t, &h);
    for (i = 0; i < h.n; i++) {
        uint32_t v = h.v[i];
        voice(SND[tr.s.sound].hz[v], V_AMP[v] * (float)h.vol[i] / (float)MT_VOL_MAX, SND[tr.s.sound].tau);
        tr.nv[v]++;
    }
}

static float gain_now;

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i, k, s0 = au_blk;
    float gain = (float)master_q12 / 4096.0f * 8300000.0f;
    s0 += APP_BLOCK;                     /* (au_blk is the last block's start: this one follows it) */
    if (!tr.run) tr.next = 0, tr.next_at = 0;
    else if (tr.next == 0 && tr.next_at == 0) tr.next_at = tick_at(0);
    gain_now += (gain - gain_now) * 0.5f;
    for (i = 0; i < n; i++) {
        float s = 0.0f;
        uint32_t now = s0 + i;
        while (tr.run && (int32_t)(now - tr.next_at) >= 0) {
            if (tr.pend && (int32_t)(tr.next_at - tr.pend_at) >= 0) {   /* a switch: another song or tempo */
                tr.s = tr.ps;
                tr.tick_q8 = tr.pend_q8;
                tr.start = tr.pend_at - q8_mul(tr.pend_tick, tr.pend_q8);
                tr.next = tr.pend_tick;
                tr.next_at = tr.pend_at;
                tr.pend = 0;
                tr.switches++;
            }
            sound(tr.next);
            tr.next++;
            tr.next_at = tick_at(tr.next);
        }
        for (k = 0; k < NV; k++) {
            voice_t *v = &vo[k];
            if (v->amp < 0.0005f) continue;
            s += v->amp * fm_sinf(v->ph * 6.2831853f);
            v->ph += v->inc;
            if (v->ph >= 1.0f) v->ph -= 1.0f;
            v->amp *= v->dec;
        }
        lr[2 * i] = lr[2 * i + 1] = (int32_t)(s * gain_now);
    }
    au_blk = s0;
    tk_since = 0;
}

void app_panic(void)
{
    uint32_t k;
    tr.run = 0;
    tr.pend = 0;
    for (k = 0; k < NV; k++) vo[k].amp = 0.0f;
}

static uint32_t now_smp(void)            /* the clock, now (main loop or tick) */
{
    uint32_t f = plat_lock(), s = au_blk + APP_BLOCK + tk_since * (SR / 1000u);
    plat_unlock(f);
    return s;
}

/* -------------------------------------------------------------- the keys --- */
#define KQ 32
static volatile uint32_t kq_key[KQ], kq_ms[KQ], kq_w;   /* key presses: which, when (the tick writes) */
static uint32_t kq_r, tk_keys;

void app_tick(void)
{
    uint32_t keys = plat_keys(), down, k;
    tk_since++;
    down = keys & ~tk_keys;
    tk_keys = keys;
    for (k = 0; down && k < NKEYS; k++)
        if (down >> k & 1u) {
            kq_key[kq_w % KQ] = k;
            kq_ms[kq_w % KQ] = plat_ms();
            kq_w++;
        }
}

static const uint8_t WHITE[16] = PLAT_WHITE_KEYS;
static int white_of(uint32_t k)          /* a key's white index 0..15, -1 for a black key */
{
    int i;
    for (i = 0; i < 16; i++)
        if (WHITE[i] == k) return i;
    return -1;
}

/* ------------------------------------------------------------------- UI --- */
#define R_HEAD 1u
#define R_ANIM 2u                        /* the main area's top half: the tempo, the pendulum or the lamps */
#define R_INFO 4u                        /* its bottom half: the songs, the steps or the mix */
#define R_FOOT 8u
#define R_ALL 15u
#define BAND 88
#define C_RED RGB(240, 70, 60)
#define C_GRN RGB(80, 220, 120)

static struct {
    uint32_t btn, redraw, anim_at;
    char toast[2][28];
    uint32_t toast_until;
    uint8_t running, edit;
    int8_t pend;                         /* SET: the song waiting for the next bar, -1 none */
    uint32_t tt[4], tt_n;                /* tap tempo: the last presses, ms */
    int32_t cur;                         /* the tick playing now, -1 stopped or before the start */
    uint32_t lit, mid, dim;              /* the keys' lights (checks) */
} ui;
static int host_fails;

static void zero(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}

static void tcat(char *d, const char *s)
{
    while (*d) d++;
    while (*s) *d++ = *s++;
    *d = 0;
}

static void tint(char *d, int v)         /* decimal */
{
    char t[12];
    int i = 0, neg = v < 0;
    unsigned u = (unsigned)(neg ? -v : v);
    do t[i++] = (char)('0' + u % 10u), u /= 10u;
    while (u);
    if (neg) t[i++] = '-';
    while (*d) d++;
    while (i) *d++ = t[--i];
    *d = 0;
}

static void toast(const char *a, const char *b)
{
    uint32_t i;
    for (i = 0; i < 27u && a[i]; i++) ui.toast[0][i] = a[i];
    ui.toast[0][i] = 0;
    for (i = 0; i < 27u && b[i]; i++) ui.toast[1][i] = b[i];
    ui.toast[1][i] = 0;
    ui.toast_until = plat_ms() + 1500u;
    ui.redraw |= R_FOOT;
}

void app_say(const char *a, const char *b)
{
    char t[64];
    toast(a ? a : "", b ? b : "");
    if (ann_on()) {
        t[0] = 0;
        tcat(t, ui.toast[0]);
        tcat(t, " ");
        tcat(t, ui.toast[1]);
        ann_say(t, 0, ANN_NOW);
    }
}

static void say_int(const char *a, int v, const char *unit)
{
    char t[24];
    t[0] = 0;
    tint(t, v);
    tcat(t, unit);
    app_say(a, t);
}

static void changed(uint32_t r)
{
    ui.redraw |= r;
    dirty_at = plat_ms();
    if (!dirty_at) dirty_at = 1u;
}

/* --------------------------------------------------------------- transport --- */
static void tr_stop(void)
{
    uint32_t f = plat_lock();
    tr.run = 0;
    tr.pend = 0;
    plat_unlock(f);
    ui.running = 0;
    ui.pend = -1;
    ui.redraw |= R_ALL;
}

static void tr_start(void)
{
    uint32_t f = plat_lock();
    tr.s = *CUR;
    tr.tick_q8 = mt_tick_q8(CUR->bpm, SR);
    tr.start = au_blk + APP_BLOCK + tk_since * (SR / 1000u) + SR / 20u;   /* 50 ms ahead: the first click whole */
    tr.next = 0;
    tr.next_at = 0;
    tr.pend = 0;
    tr.run = 1;
    plat_unlock(f);
    ui.running = 1;
    ui.pend = -1;
    ui.redraw |= R_ALL;
}

/* while playing: the active song as it is now. FOLLOW_EDIT from the next tick, the count going on (a tempo, a step);
 * FOLLOW_BAR at the next bar, its own bar starting there (another song); FOLLOW_NOW at the next tick, its bar
 * starting there */
enum { FOLLOW_EDIT, FOLLOW_BAR, FOLLOW_NOW };
static void tr_follow(int how)
{
    uint32_t f, at, t0;
    if (!ui.running) return;
    f = plat_lock();
    if (how == FOLLOW_BAR) {
        uint32_t bar = mt_bar_ticks(&tr.s);
        at = tick_at((tr.next + bar - 1u) / bar * bar);
        t0 = 0;
    } else if (tr.pend) {                /* one waits already: the same moment, the newest settings */
        at = tr.pend_at;
        t0 = how == FOLLOW_NOW ? 0u : tr.pend_tick;
    } else {
        at = tick_at(tr.next);
        t0 = how == FOLLOW_NOW ? 0u : tr.next;
    }
    tr.ps = *CUR;
    tr.pend_q8 = mt_tick_q8(CUR->bpm, SR);
    tr.pend_at = at;
    tr.pend_tick = t0;
    tr.pend = 1;
    plat_unlock(f);
}

static int32_t tick_now(void)            /* the tick playing now; -1 stopped or before the start */
{
    uint32_t f = plat_lock(), start = tr.start, q8 = tr.tick_q8, run = tr.run;
    int32_t d = (int32_t)(au_blk + APP_BLOCK + tk_since * (SR / 1000u) - start);
    plat_unlock(f);
    if (!run || d < 0 || !q8) return -1;
    return (int32_t)((float)d * 256.0f / (float)q8);
}

static float beats_now(void)             /* the beat playing now, with its fraction (the pendulum) */
{
    uint32_t f = plat_lock(), start = tr.start, q8 = tr.tick_q8, run = tr.run;
    int32_t d = (int32_t)(au_blk + APP_BLOCK + tk_since * (SR / 1000u) - start);
    plat_unlock(f);
    if (!run || d < 0 || !q8) return 0.0f;
    return (float)d * 256.0f / (float)q8 / (float)MT_TPB;
}

/* the beat now and whether it is an accent: what the lights and the lamps show (-1 stopped) */
static int beat_info(int *accent)
{
    int32_t c = ui.cur;
    uint32_t beat;
    *accent = 0;
    if (!ui.running || c < 0) return -1;
    beat = (uint32_t)c / MT_TPB;
    if (tr.s.mode == MT_GRID) {
        beat %= tr.s.bpb;
        *accent = tr.s.steps[beat * tr.s.div] == MT_ACCENT;
    } else {
        if (tr.s.beat) beat %= tr.s.beat;
        *accent = tr.s.beat && beat == 0;
    }
    return (int)beat;
}

/* -------------------------------------------------------------- the songs --- */
static int nused(void)
{
    uint32_t i;
    int n = 0;
    for (i = 0; i < MT_NSLOT; i++) n += rec.slot[i].used;
    return n;
}

static void song_say(void)
{
    char t[24];
    t[0] = 0;
    tint(t, rec.active + 1);
    tcat(t, "  ");
    tint(t, CUR->bpm);
    tcat(t, " BPM");
    app_say("SONG", t);
}

static void song_pick(int w)             /* the active song: w (from the next bar while playing, unless NOW) */
{
    rec.active = (uint8_t)w;
    if (ui.running) {
        tr_follow(rec.swap_now ? FOLLOW_NOW : FOLLOW_BAR);
        ui.pend = rec.swap_now ? -1 : (int8_t)w;
    }
    song_say();
    changed(R_ALL);
}

static void tap_reset(void) { ui.tt_n = 0; }

static void tap_tempo(uint32_t ms)       /* a press on the beat: the last four set the tempo */
{
    uint32_t t[4], k, i, bpm;
    if (ui.tt_n && ms - ui.tt[(ui.tt_n - 1) % 4] > 2000u) ui.tt_n = 0;
    ui.tt[ui.tt_n % 4] = ms;
    ui.tt_n++;
    if (ui.tt_n < 2) {
        app_say("TAP TEMPO", "...");
        return;
    }
    k = ui.tt_n < 4 ? ui.tt_n : 4;
    for (i = 0; i < k; i++) t[i] = ui.tt[(ui.tt_n - k + i) % 4];
    bpm = mt_tap_bpm(t, k);
    if (!bpm) return;
    CUR->bpm = (uint16_t)bpm;
    CUR->used = 1;
    say_int("TAP TEMPO", (int)bpm, " BPM");
    tr_follow(FOLLOW_EDIT);
    changed(R_ALL);
}

static void bpm_by(int d)
{
    CUR->bpm = (uint16_t)clamp(CUR->bpm + d, MT_BPM_MIN, MT_BPM_MAX);
    CUR->used = 1;
    tap_reset();
    say_int("BPM", CUR->bpm, "");
    tr_follow(FOLLOW_EDIT);
    changed(R_ALL);
}

static void edited(uint32_t r)           /* the active song changed: play it so, keep it */
{
    CUR->used = 1;
    tr_follow(FOLLOW_EDIT);
    changed(r);
}

/* ----------------------------------------------------------------- input --- */
static void set_page(int p)
{
    rec.page = (uint8_t)p;
    ui.edit = 0;
    tap_reset();
    app_say("PAGE", p == PG_SET ? "SET" : CUR->mode == MT_GRID ? "GRID" : "CLASSIC");
    changed(R_ALL);
}

static void key_set(uint32_t k, uint32_t ms, uint32_t btn)   /* SET: the white keys are the songs */
{
    int w = white_of(k);
    char t[24];
    if (w < 0) {                         /* a black key: tap tempo */
        tap_tempo(ms);
        return;
    }
    if (!ui.edit) {
        if (!rec.slot[w].used) {
            t[0] = 0;
            tint(t, w + 1);
            tcat(t, " EMPTY");
            app_say("SONG", t);
        } else if (w != rec.active || ui.pend >= 0)
            song_pick(w);
        return;
    }
    if (btn >> B_SEL & 1u) {             /* EDIT, SEL held: clear it (the last one stays) */
        uint32_t i;
        if (!rec.slot[w].used) return;
        if (nused() <= 1) {
            app_say("SONG", "THE LAST ONE STAYS");
            return;
        }
        mt_slot_default(&rec.slot[w]);
        t[0] = 0;
        tint(t, w + 1);
        tcat(t, " CLEARED");
        app_say("SONG", t);
        if (w == rec.active) {
            for (i = 0; i < MT_NSLOT && !rec.slot[i].used; i++) {}
            rec.active = (uint8_t)i;
            tr_follow(FOLLOW_NOW);
        }
        changed(R_ALL);
        return;
    }
    if (w != rec.active) {               /* EDIT: pick it (an empty one starts as a copy of the song on) */
        if (!rec.slot[w].used) {
            rec.slot[w] = *CUR;
            rec.slot[w].used = 1;
        }
        tap_reset();
        rec.active = (uint8_t)w;
        tr_follow(FOLLOW_NOW);
        tap_tempo(ms);                   /* (this press is the first tap) */
        song_say();
        changed(R_ALL);
        return;
    }
    tap_tempo(ms);                       /* EDIT, the same key again: its tempo */
}

static void key_pat(uint32_t k, uint32_t ms)   /* GRID: the white keys are the steps; CLASSIC: any key taps */
{
    int w = white_of(k);
    static const char *const LV[4] = { "OFF", "GHOST", "NORMAL", "ACCENT" };
    char t[24];
    if (CUR->mode == MT_CLASSIC || w < 0) {
        tap_tempo(ms);
        return;
    }
    if ((uint32_t)w >= mt_nsteps(CUR)) return;
    CUR->steps[w] = (uint8_t)((CUR->steps[w] + 1u) % 4u);
    t[0] = 0;
    tint(t, w + 1);
    tcat(t, " ");
    tcat(t, LV[CUR->steps[w]]);
    app_say("STEP", t);
    edited(R_INFO);
}

static void meter_say(void)
{
    char t[24];
    t[0] = 0;
    tint(t, CUR->bpb);
    tcat(t, "/4 ");
    tcat(t, DIV_NAME[CUR->div]);
    app_say("BAR", t);
}

static void input(void)
{
    uint32_t btn = plat_buttons(), k;
    int32_t e;
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit)) continue;
        if (k == B_HOME) set_page(rec.page == PG_SET ? PG_PAT : PG_SET);
        else if (k == B_PLAY) {
            if (ui.running) tr_stop();
            else tr_start();
        } else if (k == B_REC) {
            if (rec.page == PG_SET) {
                ui.edit = (uint8_t)!ui.edit;
                tap_reset();
                app_say("SET", ui.edit ? "EDIT" : "PLAY");
                ui.redraw |= R_ALL;
            } else
                tap_tempo(plat_ms());
        } else if (k == B_SEL && rec.page == PG_PAT) {
            CUR->mode = (uint8_t)(CUR->mode == MT_GRID ? MT_CLASSIC : MT_GRID);
            app_say("PAGE", CUR->mode == MT_GRID ? "GRID" : "CLASSIC");
            edited(R_ALL);
        } else if (k == B_OCTDN || k == B_OCTUP) {
            int d = k == B_OCTUP ? 1 : -1;
            if (rec.page == PG_PAT && CUR->mode == MT_CLASSIC) {
                CUR->beat = (uint8_t)clamp(CUR->beat + d, 0, MT_BEAT_MAX);
                if (CUR->beat) say_int("BEAT", CUR->beat, "");
                else app_say("BEAT", "NO ACCENT");
                edited(R_ALL);
            } else
                bpm_by(5 * d);
        }
    }
    ui.btn = btn;
    if ((e = plat_enc(EN_SELECT)) != 0) bpm_by(e);
    if ((e = plat_enc(EN_ALGO)) != 0) {
        CUR->sound = (uint8_t)clamp(CUR->sound + (e > 0 ? 1 : -1), 0, MT_NSND - 1);
        app_say("SOUND", SND_NAME[CUR->sound]);
        edited(R_ALL);
    }
    (void)plat_enc(EN_PRESET);
    if (rec.page == PG_SET) {
        if ((e = plat_enc(EN_K1)) != 0) {
            rec.swap_now = (uint8_t)(e > 0);
            app_say("SWITCH", rec.swap_now ? "NOW" : "NEXT BAR");
            changed(R_ALL);
        }
        (void)plat_enc(EN_K2);
        (void)plat_enc(EN_K3);
        (void)plat_enc(EN_K4);
    } else if (CUR->mode == MT_GRID) {
        if ((e = plat_enc(EN_K1)) != 0) {
            CUR->bpb = (uint8_t)clamp(CUR->bpb + (e > 0 ? 1 : -1), 1, 9);
            if (CUR->div > mt_div_max(CUR->bpb)) CUR->div = (uint8_t)mt_div_max(CUR->bpb);
            mt_grid_default(CUR);
            meter_say();
            edited(R_ALL);
        }
        if ((e = plat_enc(EN_K2)) != 0) {
            CUR->div = (uint8_t)clamp(CUR->div + (e > 0 ? 1 : -1), 1, mt_div_max(CUR->bpb));
            mt_grid_default(CUR);
            meter_say();
            edited(R_ALL);
        }
        (void)plat_enc(EN_K3);
        (void)plat_enc(EN_K4);
    } else {
        for (k = 0; k < MT_NLAYER; k++)
            if ((e = plat_enc((int)(EN_K1 + k))) != 0) {
                CUR->mix[k] = (uint8_t)clamp(CUR->mix[k] + (e > 0 ? 1 : -1), 0, MT_VOL_MAX);
                say_int(LAYER_NAME[k], CUR->mix[k], "");
                edited(R_INFO);
            }
    }
    while (kq_r != kq_w) {               /* the presses the tick dated */
        uint32_t key = kq_key[kq_r % KQ], ms = kq_ms[kq_r % KQ];
        kq_r++;
        if (rec.page == PG_SET) key_set(key, ms, btn);
        else key_pat(key, ms);
    }
}

void app_input_only(void) { input(); }

/* ---------------------------------------------------------------- lights --- */
static void lights(uint32_t now)
{
    ledfx_t kb = { 0, 0, 0 }, bb = { 0, 0, 0 };
    uint32_t k;
    int accent, beat = beat_info(&accent), flash = 0;
    int32_t c = ui.cur, step = -1;
    if (beat >= 0 && (uint32_t)c % MT_TPB < 3u) flash = accent ? 2 : 1;
    if (ui.running && c >= 0 && tr.s.mode == MT_GRID) step = mt_step_at(&tr.s, (uint32_t)c);
    ledfx_set(&bb, B_HOME, FX_GLOW, now);
    ledfx_set(&bb, B_REC, rec.page == PG_SET && ui.edit ? FX_LIT : FX_GLOW, now);
    ledfx_set(&bb, B_OCTDN, FX_GLOW, now);
    ledfx_set(&bb, B_OCTUP, FX_GLOW, now);
    if (rec.page == PG_PAT || ui.edit) ledfx_set(&bb, B_SEL, FX_GLOW, now);
    if (ui.running) bb.lit |= LED_PLAY_GREEN;
    for (k = 0; k < NKEYS; k++) {
        int w = white_of(k), fx = FX_OFF;
        if (w < 0) fx = flash == 2 ? FX_LIT : flash == 1 ? FX_MID : FX_GLOW;   /* black keys: the beat, tap here */
        else if (rec.page == PG_SET) {
            if (w == ui.pend) fx = FX_BLINK;
            else if (w == rec.active) fx = ui.edit ? FX_PULSE : FX_LIT;
            else if (rec.slot[w].used) fx = FX_GLOW;
        } else if (CUR->mode == MT_GRID) {
            static const int LV_FX[4] = { FX_OFF, FX_GLOW, FX_MID, FX_LIT };
            if ((uint32_t)w < mt_nsteps(CUR)) fx = w == step ? (CUR->steps[w] == MT_ACCENT ? FX_OFF : FX_LIT)
                                                             : LV_FX[CUR->steps[w]];
        } else
            fx = flash == 2 ? FX_LIT : flash == 1 ? FX_MID : FX_OFF;
        ledfx_set(&kb, k, fx, now);
    }
    ui.lit = kb.lit;
    ui.mid = kb.mid;
    ui.dim = kb.dim;
    ledfx_show(&bb, &kb);
}

/* ---------------------------------------------------------------- screen --- */
static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

static const char *page_name(void)
{
    if (rec.page == PG_SET) return ui.edit ? "SET: EDIT" : "SET";
    return CUR->mode == MT_GRID ? "GRID" : "CLASSIC";
}

static void draw_header(void)
{
    char t[32];
    cv_begin(240, 32, C_BLACK);
    cv_text(8, 8, &FONT_B, page_name(), rec.page == PG_SET && ui.edit ? C_WHITE : C_HI);
    t[0] = 0;
    tcat(t, "SONG ");
    tint(t, rec.active + 1);
    cv_text(232 - text_w(&FONT_T, t), 11, &FONT_T, t, C_GRAY);
    cv_rect(0, 31, 240, 1, C_LINE);
    cv_blit(0, 0);
}

/* the pendulum: a rod from a pivot below, at one side on each beat, through the middle between them */
static void draw_pendulum(int32_t cx, int32_t py, int32_t len, int32_t oy)
{
    float b = ui.running ? beats_now() : 0.0f, th, sx, cy;
    int32_t x1, y1, i, wx, wy, accent;
    int beat = beat_info(&accent);
    float ph = b - (float)(int32_t)(b / 2.0f) * 2.0f;        /* 0..2: one swing out and back */
    th = ui.running ? 0.55f * fm_cosf(FM_PI * ph) : 0.0f;
    sx = fm_sinf(th);
    cy = fm_cosf(th);
    x1 = cx + (int32_t)(sx * (float)len);
    y1 = py - (int32_t)(cy * (float)len);
    /* the scale behind it */
    for (i = -3; i <= 3; i++) {
        float a = 0.55f * (float)i / 3.0f;
        int32_t sx0 = cx + (int32_t)(fm_sinf(a) * (float)(len + 4)), sy0 = py - (int32_t)(fm_cosf(a) * (float)(len + 4));
        cv_rect(sx0 - 1, sy0 - 1 - oy, 3, 3, i == -3 || i == 3 ? C_GRAY : C_LINE);
    }
    for (i = -1; i <= 1; i++) cv_line(cx + i, py - oy, x1 + i, y1 - oy, ui.running ? C_WHITE : C_GRAY);
    /* the weight, a third of the way up: lower is faster, as on a wind-up one */
    wx = cx + (int32_t)(sx * (float)len * (0.35f + 0.4f * (float)(MT_BPM_MAX - CUR->bpm) / (float)(MT_BPM_MAX - MT_BPM_MIN)));
    wy = py - (int32_t)(cy * (float)len * (0.35f + 0.4f * (float)(MT_BPM_MAX - CUR->bpm) / (float)(MT_BPM_MAX - MT_BPM_MIN)));
    cv_rect(wx - 7, wy - 5 - oy, 15, 10, beat >= 0 && accent && ui.cur % (int32_t)MT_TPB < 3 ? C_WHITE : C_HI);
    cv_rect(cx - 14, py - oy, 29, 4, C_GRAY);
}

static void draw_tempo_left(int32_t oy)  /* SET and GRID's top: the tempo, the bar, the pendulum */
{
    char t[24];
    t[0] = 0;
    tint(t, CUR->bpm);
    text_c(62, 6 - oy, &FONT_L, t, ui.running ? C_WHITE : C_HI);
    text_c(62, 50 - oy, &FONT_T, "BPM", C_GRAY);
    t[0] = 0;
    if (CUR->mode == MT_GRID) {
        tint(t, CUR->bpb);
        tcat(t, "/4 ");
        tcat(t, DIV_NAME[CUR->div]);
    } else {
        tcat(t, "CLASSIC  BEAT ");
        tint(t, CUR->beat);
    }
    text_c(62, 66 - oy, &FONT_T, t, C_AMB);
    draw_pendulum(176, 84, 70, oy);
}

/* seven segments, as on a rhythm watch's display */
static void seg_digit(int32_t x, int32_t y, int32_t w, int32_t h, int d, uint16_t on, uint16_t off)
{
    static const uint8_t SEG[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    int32_t s = 5, hh = h / 2;
    uint8_t m = d < 0 ? 0 : SEG[d % 10];
    cv_rect(x + s, y, w - 2 * s, s, m & 0x01 ? on : off);                 /* a */
    cv_rect(x + w - s, y + s, s, hh - s, m & 0x02 ? on : off);            /* b */
    cv_rect(x + w - s, y + hh + 1, s, hh - s, m & 0x04 ? on : off);       /* c */
    cv_rect(x + s, y + h - s, w - 2 * s, s, m & 0x08 ? on : off);         /* d */
    cv_rect(x, y + hh + 1, s, hh - s, m & 0x10 ? on : off);               /* e */
    cv_rect(x, y + s, s, hh - s, m & 0x20 ? on : off);                    /* f */
    cv_rect(x + s, y + hh - s / 2, w - 2 * s, s, m & 0x40 ? on : off);    /* g */
}

static void draw_classic_top(int32_t oy)
{
    int accent, beat = beat_info(&accent), bpm = CUR->bpm, i, on = beat >= 0 && ui.cur % (int32_t)MT_TPB < 3;
    uint16_t off = C_LINE;
    for (i = 0; i < 3; i++) {            /* the tempo: three digits, a leading zero dark */
        int d = i == 0 ? bpm / 100 : i == 1 ? bpm / 10 % 10 : bpm % 10;
        seg_digit(10 + i * 36, 18 - oy, 28, 50, i == 0 && bpm < 100 ? -1 : d, C_HI, off);
    }
    cv_text(10, 72 - oy, &FONT_T, "TEMPO", C_GRAY);
    seg_digit(150, 18 - oy, 28, 50, CUR->beat, C_HI, off);   /* BEAT */
    cv_text(150, 72 - oy, &FONT_T, "BEAT", C_GRAY);
    /* the lamps: red on the accent, green on the other beats */
    cv_rect(198, 22 - oy, 16, 16, on && accent ? C_RED : RGB(60, 20, 20));
    cv_rect(198, 46 - oy, 16, 16, on && !accent ? C_GRN : RGB(20, 50, 28));
    if (beat >= 0) {
        char t[8];
        t[0] = 0;
        tint(t, beat + 1);
        cv_text(222, 34 - oy, &FONT_T, t, C_GRAY);
    }
}

static void draw_songs(int32_t oy)       /* SET's bottom: the 16 songs, as the white keys */
{
    int32_t i;
    for (i = 0; i < (int32_t)MT_NSLOT; i++) {
        int32_t x = (i % 8) * 30, y = 4 + (i / 8) * 42 - oy;
        const mt_slot_t *s = &rec.slot[i];
        char t[8];
        int on = i == rec.active, pend = i == ui.pend;
        uint16_t fg = on ? C_BLACK : s->used ? C_HI : C_DIM;
        if (on) cv_rect(x + 1, y, 28, 38, ui.edit ? C_WHITE : C_HI);
        else {
            uint16_t ln = pend ? C_WHITE : s->used ? C_DIM : C_LINE;
            cv_rect(x + 1, y, 28, 1, ln);
            cv_rect(x + 1, y + 37, 28, 1, ln);
            cv_rect(x + 1, y, 1, 38, ln);
            cv_rect(x + 28, y, 1, 38, ln);
        }
        t[0] = 0;
        tint(t, i + 1);
        text_c(x + 15, y + 3, &FONT_T, t, on ? C_BLACK : C_GRAY);
        t[0] = 0;
        if (s->used) tint(t, s->bpm);
        else tcat(t, "-");
        text_c(x + 15, y + 19, &FONT_T, t, fg);
    }
}

static void draw_steps(int32_t oy)       /* GRID's bottom: the bar, a column a step (a white key) */
{
    const mt_slot_t *s = CUR;
    int32_t n = (int32_t)mt_nsteps(s), i, w = 224 / n, x0 = 8 + (224 - w * n) / 2, play = -1;
    static const int32_t H[4] = { 0, 12, 26, 44 };
    char t[40];
    if (ui.running && ui.cur >= 0 && tr.s.mode == MT_GRID) play = mt_step_at(&tr.s, (uint32_t)ui.cur);
    for (i = 0; i < n; i++) {
        int32_t x = x0 + i * w, lv = s->steps[i], h = H[lv];
        uint16_t col = lv == MT_ACCENT ? C_WHITE : lv == MT_NORMAL ? C_HI : C_AMB;
        if (i % s->div == 0) cv_rect(x, 6 - oy, 1, 56, C_DIM);   /* a beat begins */
        cv_rect(x + 2, 58 - oy, w - 4, 2, C_LINE);
        if (h) cv_rect(x + 2, 58 - h - oy, w - 4, h, i == play ? C_GRN : col);
        else if (i == play) cv_rect(x + 2, 54 - oy, w - 4, 4, C_GRN);
        t[0] = 0;
        tint(t, i + 1);
        if (w >= 14 || i % s->div == 0) text_c(x + w / 2, 62 - oy, &FONT_T, t, i == play ? C_GRN : C_DIM);
    }
    t[0] = 0;
    tcat(t, "K1 ");
    tint(t, s->bpb);
    tcat(t, "/4   K2 ");
    tcat(t, DIV_NAME[s->div]);
    tcat(t, "   ");
    tcat(t, SND_NAME[s->sound]);
    text_c(120, 76 - oy, &FONT_T, t, C_GRAY);
}

static void draw_mix(int32_t oy)         /* CLASSIC's bottom: the four layers' volumes, K1..K4 */
{
    int32_t k, j;
    char t[24];
    for (k = 0; k < (int32_t)MT_NLAYER; k++) {
        int32_t x = 14 + k * 46;
        for (j = 0; j < MT_VOL_MAX; j++)
            cv_rect(x, 52 - j * 7 - oy, 30, 5, j < CUR->mix[k] ? (k == 0 ? C_WHITE : C_HI) : C_LINE);
        text_c(x + 15, 60 - oy, &FONT_T, LAYER_NAME[k], CUR->mix[k] ? C_HI : C_DIM);
    }
    t[0] = 0;
    tcat(t, "SOUND ");
    tcat(t, SND_NAME[CUR->sound]);
    text_c(214, 20 - oy, &FONT_T, "K1-K4", C_DIM);
    text_c(120, 76 - oy, &FONT_T, t, C_GRAY);
}

static void draw_band(int which)         /* the main area's halves: 0 the top (it moves), 1 the bottom */
{
    cv_begin(240, BAND, C_BLACK);
    if (!which) {
        if (rec.page == PG_PAT && CUR->mode == MT_CLASSIC) draw_classic_top(0);
        else draw_tempo_left(0);
    } else if (rec.page == PG_SET)
        draw_songs(0);
    else if (CUR->mode == MT_GRID)
        draw_steps(0);
    else
        draw_mix(0);
    cv_blit(0, 32 + (uint32_t)(which ? BAND : 0));
}

static void draw_footer(void)
{
    cv_begin(240, 32, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    if (plat_ms() < ui.toast_until) {
        int32_t x = cv_text(8, 10, &FONT_T, ui.toast[0], C_GRAY);
        cv_text(x + 8, 7, &FONT_B, ui.toast[1], C_HI);
    } else {
        const char *h;
        if (rec.page == PG_SET) h = ui.edit ? "KEY: PICK, TAP TEMPO   SEL+KEY: CLEAR" : "KEY: SONG   REC: EDIT   HOME: PATTERN";
        else if (CUR->mode == MT_GRID) h = "KEY: STEP LEVEL   SEL: CLASSIC";
        else h = "K1-K4: MIX   OCT: BEAT   SEL: GRID";
        cv_text(8, 10, &FONT_T, h, C_DIM);
    }
    cv_blit(0, 208);
}

static void draw(void)
{
    uint32_t r = ui.redraw;
    ui.redraw = 0;
    if (r & R_HEAD) draw_header();
    if (r & R_ANIM) {
        draw_band(0);
        ui.anim_at = plat_ms();
    }
    if (r & R_INFO) draw_band(1);
    if (r & R_FOOT) draw_footer();
}

static void speak(void)
{
    if (ann_on() && ann_where()) {
        char t[64];
        t[0] = 0;
        tcat(t, APP_NAME ". ");
        tcat(t, page_name());
        tcat(t, ". SONG ");
        tint(t, rec.active + 1);
        tcat(t, ". ");
        tint(t, CUR->bpm);
        tcat(t, " BPM");
        ann_say(t, 0, ANN_NOW);
    }
}

/* ------------------------------------------------------------ the contract --- */
void app_init(void)
{
    zero(&ui, sizeof ui);
    zero(&tr, sizeof tr);
    saves = 0;
    dirty_at = 0;
    ui.pend = -1;
    kq_r = kq_w;
    if (plat_store_load(OBJ_METRO, &rec, sizeof rec) != (int)sizeof rec || !rec_valid(&rec)) {
        zero(&rec, sizeof rec);
        defaults();
    }
    app_panic();
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = R_ALL;
}

void app_frame(void)
{
    static uint32_t last_toast, last_sw;
    static int32_t last_beat = -2, last_step = -2;
    uint32_t now = plat_ms();
    int accent;
    input();
    ui.cur = tick_now();
    if (tr.switches != last_sw) {        /* a song switch has landed */
        last_sw = tr.switches;
        if (ui.pend >= 0 && !tr.pend) ui.pend = -1, ui.redraw |= R_ALL;
    }
    {
        int32_t b = beat_info(&accent), st = ui.running && ui.cur >= 0 && tr.s.mode == MT_GRID
                                                 ? mt_step_at(&tr.s, (uint32_t)ui.cur) : -1;
        if (b != last_beat || st != last_step) ui.redraw |= R_INFO | R_ANIM;
        last_beat = b;
        last_step = st;
    }
    if (ui.running && now - ui.anim_at >= 33u) ui.redraw |= R_ANIM;   /* the pendulum, ~30 frames a second */
    if ((ui.toast_until && now >= ui.toast_until) != last_toast) {
        last_toast = ui.toast_until && now >= ui.toast_until;
        ui.redraw |= R_FOOT;
    }
    if (dirty_at && (int32_t)(now - dirty_at) > 3000) rec_save();
    lights(now);
    speak();
    ann_tick(now);
    if (ui.redraw) draw();
}

/* --------------------------------------------------------- host simulator --- */
#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    uint32_t i;
    if (!strcmp(what, "page")) *got = rec.page;
    else if (!strcmp(what, "active")) *got = rec.active;
    else if (!strcmp(what, "edit")) *got = ui.edit;
    else if (!strcmp(what, "bpm")) *got = CUR->bpm;
    else if (!strcmp(what, "mode")) *got = CUR->mode;
    else if (!strcmp(what, "bpb")) *got = CUR->bpb;
    else if (!strcmp(what, "div")) *got = CUR->div;
    else if (!strcmp(what, "beat")) *got = CUR->beat;
    else if (!strcmp(what, "sound")) *got = CUR->sound;
    else if (!strcmp(what, "swap_now")) *got = rec.swap_now;
    else if (!strcmp(what, "used")) {    /* the stored songs, bit w = white key w */
        *got = 0;
        for (i = 0; i < MT_NSLOT; i++) *got |= (int)rec.slot[i].used << i;
    } else if (!strncmp(what, "step", 4)) *got = CUR->steps[atoi(what + 4) & 15];
    else if (!strncmp(what, "mix", 3)) *got = CUR->mix[atoi(what + 3) & 3];
    else if (!strncmp(what, "slotbpm", 7)) *got = rec.slot[atoi(what + 7) & 15].bpm;
    else if (!strcmp(what, "running")) *got = ui.running;
    else if (!strcmp(what, "pend")) *got = ui.pend;
    else if (!strcmp(what, "play_bpm")) *got = tr.s.bpm;   /* what the render plays now */
    else if (!strcmp(what, "switches")) *got = (int)tr.switches;
    else if (!strcmp(what, "n_accent")) *got = (int)tr.nv[MT_V_ACCENT];
    else if (!strcmp(what, "n_normal")) *got = (int)tr.nv[MT_V_NORMAL];
    else if (!strcmp(what, "n_ghost")) *got = (int)tr.nv[MT_V_GHOST];
    else if (!strcmp(what, "n_8")) *got = (int)tr.nv[MT_V_8];
    else if (!strcmp(what, "n_3")) *got = (int)tr.nv[MT_V_3];
    else if (!strcmp(what, "n_16")) *got = (int)tr.nv[MT_V_16];
    else if (!strcmp(what, "lit_keys")) *got = (int)ui.lit;
    else if (!strcmp(what, "mid_keys")) *got = (int)ui.mid;
    else if (!strcmp(what, "dim_keys")) *got = (int)ui.dim;
    else if (!strcmp(what, "saves")) *got = (int)saves;
    else return 0;
    return 1;
}

int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    (void)b, (void)c, (void)d;
    if (!strcmp(cmd, "count_reset")) {   /* count_reset: the voice counters back to 0 */
        uint32_t i, f = plat_lock();
        for (i = 0; i < MT_NV; i++) tr.nv[i] = 0;
        plat_unlock(f);
        (void)a;
        return 1;
    }
    return 0;
}

int app_host_fails(void) { return host_fails; }
#endif
