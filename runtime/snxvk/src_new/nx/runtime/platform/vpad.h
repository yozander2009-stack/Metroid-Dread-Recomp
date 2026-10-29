/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen controls: a pad drawn over the game (ui.h) that fingers or the mouse press, with rEden's touch
   overlay's sprites, sizes and options, laid out as the console's two controllers hold them. The same on every
   platform: on by default on Android, off elsewhere. Kept in the game's .ini (keys controls and controls_*) */
#ifndef SNX_VPAD_H
#define SNX_VPAD_H
#include "nx/runtime/platform/ui.h"
#include "nx/runtime/ini.h"
#include "snx_settings.h"

/* in the order they are drawn: a finger presses the last one drawn under it */
enum {
    SNX_VPAD_L,
    SNX_VPAD_R,
    SNX_VPAD_ZL,
    SNX_VPAD_ZR,
    SNX_VPAD_MINUS,
    SNX_VPAD_PLUS,
    SNX_VPAD_CAPTURE,
    SNX_VPAD_HOME,
    SNX_VPAD_L3,
    SNX_VPAD_R3,
    SNX_VPAD_DPAD,
    SNX_VPAD_STICK_L,
    SNX_VPAD_STICK_R,
    SNX_VPAD_A,
    SNX_VPAD_B,
    SNX_VPAD_X,
    SNX_VPAD_Y,
    SNX_VPAD_COUNT
};
/* what the frontend does, read with vpad_actions */
#define SNX_VPAD_DO_MENU 1u /* Home was pressed: the game's menu */
#define SNX_VPAD_DO_SHOT 2u /* Capture was pressed: a screenshot */
#define SNX_VPAD_DO_BUZZ 4u /* a control went down or up: a short buzz, haptics being on */

typedef struct {
    float x, y; /* its middle, 0 to 1 across the window's clear part */
    int size;   /* percent of its own default size */
} SnxVpadPlace;
/* the controls' layout, read from their places (controls_a...). Their other settings are g_settings' (settings.txt):
   show_controls, controls_scale (rEden's 0 to 100: every control at (scale + 50)%), controls_opacity, snap_to_grid,
   relative_stick_center, d_pad_slide, haptics, and control_shown[SNX_VPAD_] */
typedef struct {
    SnxVpadPlace at[SNX_VPAD_COUNT];
} SnxVpadConfig;
/* the layout, its defaults filled in on first use */
SnxVpadConfig* vpad_config(void);
#define SNX_VPAD_SIZE_MIN 50
#define SNX_VPAD_SIZE_MAX 200

void vpad_defaults(SnxVpadConfig* c); /* the default layout */
/* places, sizes and the shown ones back to their defaults, kept */
void vpad_reset_layout(void);
/* the settings' tables (settings.h): g_vpad_ini, controls, controls_scale..., a control's place (controls_a); and
   g_vpad_shown_ini, a control shown or not (controls_a_shown), in the order of SNX_VPAD_ */
/* the game's menu's page of these settings and its page of the buttons shown, as the launcher's Settings name them */
#define SNX_VPAD_PAGE    "On-screen controls"
#define SNX_VPAD_BUTTONS "Buttons"
/* a key of the game's .ini (controls, controls_scale..., controls_a, controls_a_shown...): applied, 1; not one of
   these, 0 */
int vpad_ini(const char* key, const char* value);
/* where changed settings go: called with each key and its value (the game's .ini) */
void vpad_set_store(void (*store)(const char* key, const char* value));
/* a setting's current value to the store, named by its variable (&g_settings.controls_scale); a control's place
   with vpad_store_control, whether it is shown with vpad_store_shown */
void vpad_store(const int* setting);
void vpad_store_opt(const IniOpt* o); /* any table's setting, as its variable holds it */
void vpad_store_control(int control);
void vpad_store_shown(int control);

/* Fingers, and the mouse as one, in window pixels; safe is the window's clear part (x, y, w, h). A finger that lands
   on a control, or any in edit mode, is the controls': 1, and its moves and lift are theirs */
int vpad_down(u64 id, float x, float y, const int safe[4]);
void vpad_move(u64 id, float x, float y, const int safe[4]);
int vpad_up(u64 id);
void vpad_release(void); /* every finger off: the menu opened, the window went away */
/* the controls' buttons as HidNpadButton bits, and their sticks (lx, ly, rx, ry) from -32767 to 32767, right and up;
   a button pressed and let go since the last call is in the bits this once */
void vpad_state(u64* buttons, i32 sticks[4]);
unsigned vpad_actions(void); /* SNX_VPAD_DO_ bits since the last call */

/* edit mode: a control dragged moves, one tapped can be sized from the bar on top, Done (or vpad_edit(0)) ends it */
void vpad_edit(int on);
int vpad_editing(void);
int vpad_shown(void); /* drawn: on, or in edit mode */

/* safe in the pixels of the picture drawn on */
void vpad_draw(SnxUiList* l, const int safe[4]);

/* Showcase (settings.txt's showcase): player 1's buttons (HidNpadButton bits) and sticks (-32767 to 32767, up
   positive) as the game got them, drawn with these controls' sprites in showcase_corner's corner of safe, and the
   frames a second (fps, below 0 for none yet) over them. Both under ui_lock */
void vpad_showcase_input(u64 buttons, const i32 sticks[4]);
/* the frames a second the showcase shows: the game's presents (a count that only grows) over the last half second or
   more, at now_ns; -1 until the first half second is over */
int vpad_showcase_fps(u32 presents, u64 now_ns);
void vpad_showcase_draw(SnxUiList* l, const int safe[4], int fps);
/* the showcase's box on safe (x, y, w, h into box) and its corner, 0 top left, 1 top right, 2 bottom left, 3 bottom
   right; -1 while it is off */
int vpad_showcase_box(const int safe[4], float box[4]);
/* a corner's word, tl tr bl br, as showcase_corner and stats_corner take them: its index as above, -1 for another */
int vpad_corner(const char* word);
const char* vpad_corner_word(int corner);
#endif
