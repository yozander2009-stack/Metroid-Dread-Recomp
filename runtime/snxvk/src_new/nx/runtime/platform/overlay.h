/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu: Android's back button, a gamepad's Home, the on-screen Home or F2 opens it over the game, in the
   menu's look (src/gui, the YoRHa theme), drawn by the renderer as quads (ui.h). The same on every platform: the
   on-screen controls' options (vpad.h), the touch screen's, and Exit */
#ifndef SNX_OVERLAY_H
#define SNX_OVERLAY_H
#include "nx/runtime/platform/ui.h"

#define OVERLAY_COLS     40 /* the panel, in cells of the 8x16 font: the narrowest */
#define OVERLAY_COLS_MAX 72 /* and the widest, as far as three quarters of the picture let it */
#define OVERLAY_ROWS     16 /* the tallest page's: the others are as tall as their items */
/* what a menu action leaves to its caller: the save state ones for the slot overlay_slot names; the keyboard's box
   answered (overlay_keyboard_take), or a tap on its line, which asks for the system's keyboard again */
enum {
    OVERLAY_NONE,
    OVERLAY_CLOSE,
    OVERLAY_EXIT,
    OVERLAY_EDIT,
    OVERLAY_SAVE,
    OVERLAY_LOAD,
    OVERLAY_DELETE,
    OVERLAY_KEYBOARD,
    OVERLAY_TYPE
};

/* whether it is open, for the renderer's thread too */
int overlay_shown(void);
void overlay_toggle(void);
/* the display's own scale (SDL's: dpi / 160 on a phone), which the panel's letters follow; 1 until told */
void overlay_set_display_scale(float scale);
int overlay_display_percent(void); /* that scale, in percent */
/* where the panel goes on a picture of sw x sh pixels: its corner and the whole number it is scaled by */
void overlay_place(int sw, int sh, int* x, int* y, int* scale);
int overlay_select(void);      /* the focused item's action: a page, a setting changed, or an OVERLAY_ for the caller */
void overlay_move(int rows);   /* the focus up (-1) or down (1), round the page */
void overlay_scroll(int rows); /* the mouse's wheel: the same, stopping at the ends */
void overlay_side(int step);   /* the focused setting down (-1) or up (1) */
int overlay_back(void);        /* the page above; OVERLAY_CLOSE from the first */
/* a button pressed, as the game reads it (one HidNpadButton bit): the D-pad and the left stick move, A selects, B goes
   back, the rest do nothing. What it did, as overlay_select */
int overlay_pad(u64 button);
/* the pads each poll, the on-screen controls and the connected ones: their buttons and sticks at ms. With the menu
   shown, each button pressed since the last call acts as overlay_pad, the left stick past half way as the D-pad; a
   direction held goes on after SNX_OVERLAY_REPEAT_DELAY ms, every SNX_OVERLAY_REPEAT_MS; a press held as the menu opens
   does not act. What the last one did, or the first that leaves the menu */
#define SNX_OVERLAY_REPEAT_DELAY 400
#define SNX_OVERLAY_REPEAT_MS    120
int overlay_controls(u64 buttons, const i32 sticks[4], u64 ms);
/* the button map page waits for a control pressed on a pad (pad_map.h's): it takes it, and that press is no menu
   key */
int overlay_listening(void);
void overlay_press(int control);
/* a tap at x, y on a picture of sw x sh: what it did, OVERLAY_CLOSE outside the panel */
int overlay_tap(float x, float y, int sw, int sh);
/* a finger or the mouse (touch.h's SNX_TOUCH_MOUSE) on the open menu, at x, y in the picture's pixels: down on an
   on-screen control it is the controls', whose presses move the menu (overlay_controls); else its lift taps the menu
   where it lifts (what that did). Reset forgets the one down, which lifts without a tap; cancel forgets id's */
void overlay_pointer_down(u64 id, float x, float y, const int safe[4]);
void overlay_pointer_move(u64 id, float x, float y, const int safe[4]);
int overlay_pointer_up(u64 id, float x, float y, int sw, int sh);
void overlay_pointer_cancel(u64 id);
void overlay_pointers_reset(void);
/* the menu, and over the game a notification until its time is up, at now_ns (plat_ns) */
void overlay_draw(SnxUiList* l, int sw, int sh, u64 now_ns);
/* a notification of one line in the picture's corner for SNX_OVERLAY_TOAST_MS from now_ns: a save state saved or
   loaded, a screenshot taken. Under ui_lock */
#define SNX_OVERLAY_TOAST_MS 3000
void overlay_toast(const char* text, u64 now_ns);
int overlay_toasting(u64 now_ns); /* one shows */
/* The save states' pages: a row per slot, the focused one's thumbnail beside them, and Save, Load and Delete for one
   (Load and Delete asked first). Slot 0 is the one a load saves over first, which only loads or goes */
#define OVERLAY_SLOTS_MAX 33
typedef struct {
    char when[32]; /* when it was saved, and the play time then; "" for an empty slot */
    char why[96];  /* why it can't be loaded, "" when it can */
    u8* thumb;     /* SNX_UI_IMAGE_W x SNX_UI_IMAGE_H RGBA, NULL for none: allocated, the overlay's to free */
} OverlaySlot;
/* the slots listed, n of them from slot 0, taken over, under ui_lock: the frontend's as each page opens and after a
   save or a delete */
void overlay_slots_set(const OverlaySlot* slots, int n);
int overlay_slot(void);        /* the slot the last OVERLAY_SAVE, _LOAD or _DELETE is for */
int overlay_states_shown(void); /* one of the save states' pages shows: the game waits meanwhile */
int overlay_states_stale(void); /* the slots page opened since the last overlay_slots_set: its list is read again */
/* the columns the menu's pages and the stopped panel take at scale on a picture sw wide: as many as three quarters of it
   holds, within OVERLAY_COLS and OVERLAY_COLS_MAX */
int overlay_cols(int sw, int scale);
/* a fatal error's panel (backend/sdl3.c), a dialog as the menu's pages are: the head, the game's name, the error, what it
   means and what to do, cut to its width, then Share Log, Copy path and Close, one in focus */
typedef struct {
    float x, y, w, h;
} SnxOverlayRect;
enum { SNX_OVERLAY_ERROR_SHARE, SNX_OVERLAY_ERROR_COPY, SNX_OVERLAY_ERROR_CLOSE, SNX_OVERLAY_ERROR_ITEMS };
typedef struct {
    const char *name, *msg, *how;
    const char* log;  /* the game's log, underlined as a link; NULL for none */
    const char* said; /* what Share Log or Copy path did, over the items; NULL before either */
    int focus;        /* one of SNX_OVERLAY_ERROR_SHARE, _COPY and _CLOSE */
    /* drawn: where the log's link and each item are, empty when not shown */
    SnxOverlayRect link, items[SNX_OVERLAY_ERROR_ITEMS];
} SnxOverlayError;
void overlay_error_draw(SnxUiList* l, int sw, int sh, SnxOverlayError* e);
/* 1 when x, y falls in r, an empty one taking nothing */
int overlay_error_hit(const SnxOverlayRect* r, float x, float y);

/* The software keyboard's box, for a game that asks the player for text (am.c, through backend_keyboard_show): a
   dialog over the game, the same on every platform, with the game's header, sub and guide texts, the line the player
   types into, how many characters it holds against the game's limits, and the game's OK and Cancel. What the
   window's text input types (a keyboard, an input method, the system's on-screen keyboard) is added to the line up to
   its most; Backspace takes its last character away; Enter, A or a tap on OK gives the line, but one short of its
   least, which the box then says; Esc, B or a tap on Cancel cancels. While it shows and the menu doesn't, it takes
   the keys, the buttons and the fingers, and the menu goes over it. Under ui_lock */
#define OVERLAY_KB_UNITS 1024                       /* the most UTF-16 units a line holds, as the game counts them */
#define OVERLAY_KB_TEXT  (3 * OVERLAY_KB_UNITS + 1) /* its bytes in UTF-8, a 0 after them */
typedef struct {
    char text[OVERLAY_KB_TEXT]; /* the game's initial text, UTF-8: the line starts as it, cut to max */
    /* the game's texts, UTF-8, "" for none: over the line, under it and the OK item's label */
    char header[3 * 64 + 1], sub[3 * 128 + 1], guide[3 * 256 + 1], ok[3 * 8 + 1];
    /* the line's UTF-16 units: the least it is given with, and the most (0, or past OVERLAY_KB_UNITS: that) */
    u32 min, max;
    int password; /* each character drawn as an asterisk */
    int numbers;  /* the game's number pad: a system keyboard of digits */
    u32 ticket;   /* the asker's, handed back with the answer */
} OverlayKeyboard;
/* the box for k, in place of one that shows */
void overlay_keyboard_show(const OverlayKeyboard* k);
/* the box for ticket taken away unanswered, as the game takes its keyboard away; another one stays */
void overlay_keyboard_hide(u32 ticket);
int overlay_keyboard_shown(void); /* for any thread */
/* text the window's text input typed, UTF-8: as many of its characters as the line has room for, controls left out */
void overlay_keyboard_type(const char* text);
/* a key on the line: OVERLAY_KEYBOARD once it answers (the box gone), else OVERLAY_NONE */
enum { OVERLAY_KEY_BACKSPACE, OVERLAY_KEY_ENTER, OVERLAY_KEY_CANCEL };
int overlay_keyboard_key(int key);
/* the answer, each taken once: 0 with the line in out (size bytes), 1 cancelled, and the ticket it is for; -1 for none */
int overlay_keyboard_take(u32* ticket, char* out, size_t size);
/* what the window's text input needs while the box shows (1; 0 when it doesn't): the box's ticket, its kind of text,
   and its line on a picture sw x sh, in its pixels, where an input method puts its window */
typedef struct {
    u32 ticket;
    int password, numbers;
    SnxOverlayRect line;
} OverlayTyping;
int overlay_keyboard_typing(int sw, int sh, OverlayTyping* t);

#endif
