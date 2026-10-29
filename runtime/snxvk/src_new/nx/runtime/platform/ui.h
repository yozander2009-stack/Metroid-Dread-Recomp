/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the renderer draws over the game's picture: the on-screen controls (vpad.c) and the game's menu (overlay.c),
   as quads of one atlas blended over the frame. The atlas is dist/controls/controls.png with the menu's 8x16 font
   and a white block for plain fills in the places its header keeps for them, and under it the picture of the user
   playing, which the menu's head shows by their nickname */
#ifndef SNX_UI_H
#define SNX_UI_H
#include "common/cpu/aarch64/cpu.h"

typedef struct {
    float x, y, w, h; /* where, in the pixels of the picture drawn on */
    u16 u, v, uw, vh; /* the atlas texels drawn there */
    u32 rgba;         /* each texel times this colour, R G B A from the low byte */
    u8 turn;          /* the texels turned this many quarters clockwise */
    u8 sample;        /* how: UI_BLEND, UI_SHARP or UI_IMAGE */
} SnxUiQuad;
/* the atlas blended, the atlas's nearest texel (the font at whole steps), or ui_image's picture blended, its texels
   in u, v, uw and vh */
enum { UI_BLEND, UI_SHARP, UI_IMAGE };
typedef struct {
    SnxUiQuad* q;
    int n, max; /* quads past max are dropped */
} SnxUiList;

/* a sprite of controls_atlas.h at alpha (0 to 255) */
void ui_sprite(SnxUiList* l, int sprite, float x, float y, float w, float h, u32 alpha, int turn);
/* a plain rectangle */
void ui_fill(SnxUiList* l, float x, float y, float w, float h, u32 rgba);
/* the picture of the user playing (common/utilities/users.h), side pixels square at x, y: drawn SNX_UI_PICTURE texels
   square in the YoRHa theme's [head] picture shape, square or round (the rest transparent); 0 when the theme shows
   none, which draws nothing */
#define SNX_UI_PICTURE 96
int ui_picture(SnxUiList* l, float x, float y, float side);
/* ASCII text, 0xFE the font's square, in 8x16 glyphs at a whole scale; returns the width drawn */
float ui_text(SnxUiList* l, float x, float y, int scale, const char* s, u32 rgba);
/* an icon of the menus' (gui/icons.h), two of the font's cells at a whole scale, as the launcher draws it beside a
   label; returns the width drawn, 0 for GUI_ICON_NONE. Its texels are in the atlas beside the picture, on its rows:
   icon i's 16x16 from x SNX_UI_ICONS_X + 16 * i */
#define SNX_UI_ICONS_X SNX_UI_PICTURE
float ui_icon(SnxUiList* l, float x, float y, int scale, unsigned icon, u32 rgba);
/* One picture apart from the atlas, SNX_UI_IMAGE_W x SNX_UI_IMAGE_H: the menu's save state thumbnail. ui_image draws it
   at x, y, w x h: 0 while there is none, which draws nothing */
#define SNX_UI_IMAGE_W 320
#define SNX_UI_IMAGE_H 180
int ui_image(SnxUiList* l, float x, float y, float w, float h);
/* the picture's texels, R G B A bytes, opaque, copied (NULL: none), under ui_lock */
void ui_image_set(const u8* rgba);
/* the picture for the renderer, under ui_lock: its texels, NULL for none, and version counting the pictures given */
const u8* ui_image_get(u32* version);

/* The atlas, each texel the bytes R G B A (a little-endian u32) with alpha premultiplied, made from the sprites'
   image when the window opens (sdl3.c decodes it to the same bytes, SDL's RGBA32), the users read by then, and freed
   by ui_atlas_done once the renderer has it, both under ui_lock. It is the image's width and SNX_UI_PICTURE rows
   taller. version counts the images given, 0 for none yet */
void ui_atlas_set(const u32* sprites, int w, int h);
const u32* ui_atlas(int* w, int* h, u32* version);
void ui_atlas_done(void);

/* the controls and the menu change on the main thread while the renderer draws them on its own: both under this */
void ui_lock(void);
void ui_unlock(void);
#endif
