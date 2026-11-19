/* SPDX-License-Identifier: GPL-3.0-or-later */
/* METRONOME: the FM-1 as a stage metronome (design: docs/dev/metronome.md in github.com/daviddwlee84/M-VAVE_FM-1).
 * v0.1: SET, a setlist of 16 presets on the white keys (a song's tempo, meter, pattern and sound; the stored keys
 * glow, the one playing is lit; switch songs at the next bar). GRID, the bar's steps on the white keys, each off,
 * ghost, normal or accent, which is how bright its key is. CLASSIC, a rhythm-watch layout: the quarter, eighth,
 * triplet and sixteenth layers mixed on K1-K4, the accent every BEAT beats. Tap tempo on the black keys.
 * The timing and the patterns are metro.c (platform-free). Built on fm1-base (MPL-2.0, its headers). This app is
 * GPL-3.0-or-later; ../common (ledfx) is MIT. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "METRONOME"
#define APP_SPLASH_RGB RGB(240, 200, 90)
#define APP_PALETTE 1
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TICK                         /* 1 kHz: the keys' presses, timed for tap tempo */

/* the settings and the 16 songs, one record (app.c rec_t), at 0xCE000. 0xD0000..0xDE000 are all taken by this
 * repository's other apps and the panel (docs/hardware/flash-map.md). Check it is blank in the latest backup with
 * just emu-rehearse before a flash. The panel calibration is the platform's */
enum { OBJ_PANEL, OBJ_METRO, OBJ_NOBJ };
#define APP_STORE_MAP {0xDC000u, 0xCE000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
void app_tick(void);

/* host simulator: checks (page, active, bpm, used, step3, n_accent, ...) */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
#define APP_HOST_CMD
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);
#define APP_HOST_FAILS
int app_host_fails(void);
