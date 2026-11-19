/* SPDX-License-Identifier: GPL-3.0-or-later */
/* metro.c on this computer: just metro-unit. Slots, GRID and CLASSIC timing, tap tempo. */
#include <stdio.h>
#include "../metro.c"

static int fails, checks;
#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static int count(const mt_slot_t *s, uint32_t ticks, int v)   /* how often voice v sounds in these ticks */
{
    uint32_t t, i;
    int n = 0;
    mt_hits_t h;
    for (t = 0; t < ticks; t++) {
        mt_hits(s, t, &h);
        for (i = 0; i < h.n; i++) n += h.v[i] == v;
    }
    return n;
}

int main(void)
{
    mt_slot_t s;
    mt_hits_t h;
    uint32_t t, ms[4];
    int bpb, d;

    CHECK(sizeof(mt_slot_t) == 28, "slot is %u bytes", (unsigned)sizeof(mt_slot_t));
    mt_slot_default(&s);
    CHECK(mt_slot_valid(&s), "default valid");
    CHECK(mt_nsteps(&s) == 4 && mt_bar_ticks(&s) == 48, "4/4 quarters");
    /* default GRID: an accent on 1, normal on 2 3 4, nothing between */
    CHECK(count(&s, 48, MT_V_ACCENT) == 1 && count(&s, 48, MT_V_NORMAL) == 3 && count(&s, 48, MT_V_GHOST) == 0,
          "default bar");
    mt_hits(&s, 0, &h);
    CHECK(h.n == 1 && h.v[0] == MT_V_ACCENT, "accent on tick 0");
    mt_hits(&s, 1, &h);
    CHECK(h.n == 0, "nothing on tick 1");
    mt_hits(&s, 48, &h);
    CHECK(h.n == 1 && h.v[0] == MT_V_ACCENT, "accent on the next bar");

    /* every meter and division: the steps fit, each step sounds once a bar, on its own tick */
    for (bpb = 1; bpb <= 9; bpb++)
        for (d = 1; d <= mt_div_max(bpb); d++) {
            uint32_t n;
            if (d == 3 && bpb * 3 > 16) continue;
            s.bpb = (uint8_t)bpb;
            s.div = (uint8_t)d;
            mt_grid_default(&s);
            n = mt_nsteps(&s);
            CHECK(n <= MT_STEPS, "%d x %d fits", bpb, d);
            CHECK(mt_slot_valid(&s), "%d x %d valid", bpb, d);
            CHECK(count(&s, mt_bar_ticks(&s), MT_V_ACCENT) == 1, "%d x %d one accent", bpb, d);
            CHECK(count(&s, mt_bar_ticks(&s), MT_V_NORMAL) == bpb - 1, "%d x %d beats", bpb, d);
            CHECK(count(&s, mt_bar_ticks(&s), MT_V_GHOST) == (int)n - bpb, "%d x %d ghosts", bpb, d);
            for (t = 0; t < mt_bar_ticks(&s); t++)
                CHECK(mt_step_at(&s, t) == (int32_t)(t / (MT_TPB / (uint32_t)d)), "%d x %d step at %u", bpb, d,
                      (unsigned)t);
        }
    CHECK(mt_div_max(4) == 4 && mt_div_max(5) == 3 && mt_div_max(6) == 2 && mt_div_max(9) == 1, "div max");

    /* a GRID step set off is silent; triplets land on ticks 0, 4, 8 */
    s.bpb = 4;
    s.div = 3;
    mt_grid_default(&s);
    s.steps[1] = MT_OFF;
    CHECK(count(&s, 48, MT_V_GHOST) == 7, "one triplet ghost off");
    mt_hits(&s, 8, &h);
    CHECK(h.n == 1 && h.v[0] == MT_V_GHOST, "third triplet on tick 8");
    s.steps[11] = MT_ACCENT;
    mt_hits(&s, 44, &h);
    CHECK(h.n == 1 && h.v[0] == MT_V_ACCENT, "an accent set on the last triplet");

    /* CLASSIC: layers mix as on a rhythm watch */
    mt_slot_default(&s);
    s.mode = MT_CLASSIC;
    s.beat = 3;
    s.mix[MT_L8] = 4;
    s.mix[MT_L16] = 2;
    CHECK(mt_bar_ticks(&s) == 36, "BEAT 3: a bar of 3");
    CHECK(count(&s, 36, MT_V_ACCENT) == 1 && count(&s, 36, MT_V_NORMAL) == 2, "accent every 3");
    CHECK(count(&s, 36, MT_V_8) == 3 && count(&s, 36, MT_V_16) == 9 && count(&s, 36, MT_V_3) == 0, "layers");
    mt_hits(&s, 6, &h);
    CHECK(h.n == 2 && h.v[0] == MT_V_8 && h.vol[0] == 4 && h.v[1] == MT_V_16 && h.vol[1] == 2, "8th and 16th at 6");
    s.beat = 0;
    CHECK(count(&s, 48, MT_V_ACCENT) == 0 && count(&s, 48, MT_V_NORMAL) == 4, "BEAT 0: no accent");
    s.mix[MT_L4] = 0;
    CHECK(count(&s, 48, MT_V_NORMAL) == 0, "quarter volume 0: silent");
    s.mix[MT_L3] = 7;
    CHECK(count(&s, 48, MT_V_3) == 8, "triplets between the beats");

    /* invalid slots */
    mt_slot_default(&s);
    s.bpm = 29;
    CHECK(!mt_slot_valid(&s), "bpm 29");
    mt_slot_default(&s);
    s.bpb = 5;
    s.div = 4;
    CHECK(!mt_slot_valid(&s), "20 steps");
    mt_slot_default(&s);
    s.steps[3] = 4;
    CHECK(!mt_slot_valid(&s), "level 4");

    /* tempo */
    CHECK(mt_tick_q8(120, 44100) == 44100u * 60u * 256u / 120u / 12u, "tick at 120");
    CHECK(mt_tick_q8(300, 44100) > 0 && mt_tick_q8(30, 44100) == 44100u * 60u * 256u / 30u / 12u, "tick range");
    ms[0] = 1000, ms[1] = 1500, ms[2] = 2000, ms[3] = 2500;
    CHECK(mt_tap_bpm(ms, 4) == 120, "tap 120 got %u", (unsigned)mt_tap_bpm(ms, 4));
    CHECK(mt_tap_bpm(ms, 2) == 120 && mt_tap_bpm(ms, 1) == 0, "tap 2 and 1");
    ms[1] = 1490, ms[2] = 2010, ms[3] = 2500;
    CHECK(mt_tap_bpm(ms, 4) == 120, "uneven taps average");
    ms[1] = 1100;
    CHECK(mt_tap_bpm(ms, 2) == 300, "too fast clamps");
    ms[1] = 9000;
    CHECK(mt_tap_bpm(ms, 2) == 30, "too slow clamps");

    /* meter presets: each a valid bar, it is the one it matches, the eighths ones as written */
    {
        int i;
        for (i = 0; i < MT_NPRESET; i++) {
            mt_slot_default(&s);
            mt_preset_apply(&s, i);
            CHECK(mt_slot_valid(&s), "preset %s valid", mt_preset_name(i));
            CHECK(mt_preset_match(&s) == i, "preset %s matches %d", mt_preset_name(i), mt_preset_match(&s));
            CHECK(s.steps[0] == MT_ACCENT, "preset %s accents 1", mt_preset_name(i));
        }
        mt_slot_default(&s);
        CHECK(mt_preset_match(&s) == 2, "the default bar is 4/4");
        mt_preset_apply(&s, 4);
        CHECK(s.bpb == 2 && s.div == 3 && !mt_preset_eighths(4), "6/8 is two dotted quarters");
        mt_preset_apply(&s, 5);
        CHECK(s.bpb == 6 && s.div == 1 && s.steps[3] == MT_NORMAL && s.steps[1] == MT_GHOST && mt_preset_eighths(5),
              "6/8 in 6: 1 and 4");
        mt_preset_apply(&s, 9);
        CHECK(s.bpb == 7 && s.steps[2] == MT_NORMAL && s.steps[4] == MT_NORMAL && s.steps[6] == MT_GHOST
                  && s.steps[7] == MT_OFF, "7/8 2+2+3");
        s.steps[1] = MT_NORMAL;
        CHECK(mt_preset_match(&s) == -1, "an edited bar is no preset");
        mt_preset_apply(&s, 7);
        CHECK(mt_nsteps(&s) == 12 && count(&s, 48, MT_V_ACCENT) == 1 && count(&s, 48, MT_V_NORMAL) == 3
                  && count(&s, 48, MT_V_GHOST) == 8, "12/8 sounds 1 + 3 + 8");
    }

    /* tempo markings: every BPM in exactly one range, the ranges in order, the edges where the table says */
    {
        static const int EDGE[MT_NMARK] = { MT_BPM_MIN, 40, 60, 66, 76, 108, 120, 168, 200 };
        static const char *const NAME[MT_NMARK] = { "LARGHISSIMO", "LARGO",   "LARGHETTO", "ADAGIO",     "ANDANTE",
                                                    "MODERATO",    "ALLEGRO", "PRESTO",    "PRESTISSIMO" };
        int i, b, prev = 0, j;
        for (i = 0; i < MT_NMARK; i++) {
            const char *a = mt_marking_name(i), *e = NAME[i];
            while (*a && *a == *e) a++, e++;
            CHECK(!*a && !*e, "marking %d named %s", i, mt_marking_name(i));
            CHECK(mt_marking(EDGE[i]) == i, "%d starts %s", EDGE[i], NAME[i]);
            if (i) CHECK(mt_marking(EDGE[i] - 1) == i - 1, "%d ends %s", EDGE[i] - 1, NAME[i - 1]);
            CHECK(mt_marking(mt_marking_typical(i)) == i, "%s's typical tempo %d inside it", NAME[i],
                  mt_marking_typical(i));
        }
        for (b = MT_BPM_MIN; b <= MT_BPM_MAX; b++) {
            i = mt_marking(b);
            CHECK(i >= prev && i - prev <= 1, "%d: marking %d after %d", b, i, prev);
            prev = i;
            /* a jump up lands higher and in this range or the next; down, lower and in this one or the one before */
            j = mt_marking_jump(b, 1);
            CHECK(b == mt_marking_typical(MT_NMARK - 1) || b > mt_marking_typical(MT_NMARK - 1)
                      ? j == b : j > b && (mt_marking(j) == i || mt_marking(j) == i + 1),
                  "%d up -> %d", b, j);
            j = mt_marking_jump(b, -1);
            CHECK(b <= mt_marking_typical(0) ? j == b : j < b && (mt_marking(j) == i || mt_marking(j) == i - 1),
                  "%d down -> %d", b, j);
        }
        CHECK(mt_marking(MT_BPM_MAX) == MT_NMARK - 1, "300 is PRESTISSIMO");
        /* the walk the K2 knob makes from 120: up through the typical tempos, then down again */
        CHECK(mt_marking_jump(120, 1) == 132 && mt_marking_jump(132, 1) == 184 && mt_marking_jump(184, 1) == 208,
              "up from 120");
        CHECK(mt_marking_jump(120, -1) == 112 && mt_marking_jump(112, -1) == 92 && mt_marking_jump(92, -1) == 70
                  && mt_marking_jump(70, -1) == 63 && mt_marking_jump(63, -1) == 50 && mt_marking_jump(50, -1) == 35,
              "down from 120");
        CHECK(mt_marking_jump(140, -1) == 132, "down from inside ALLEGRO lands on its typical tempo first");
        CHECK(mt_marking_jump(208, 1) == 208 && mt_marking_jump(35, -1) == 35, "ends stay");
    }

    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
