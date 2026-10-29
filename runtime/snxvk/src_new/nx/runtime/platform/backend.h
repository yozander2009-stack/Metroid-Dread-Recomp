/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The window backend's side of the runtime (backend/sdl3.c, windowed or headless): window, input, presenting, and
   the window half of the Vulkan renderer. */
#ifndef SNX_BACKEND_H
#define SNX_BACKEND_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/runtime/ini.h"
#include "nx/runtime/platform/overlay.h"

/* the window, titled after the game and, when addons is not "", the updates and DLCs it plays with ("Update 1.2.0,
   DLC 1") */
void video_init(const char* title, const char* addons);
void video_poll(void);     /* events and input, once per main loop pass */
void frontend_frame(void); /* screenshots and the unattended limits, after each pass */

/* Offers a .ini or command line setting (the touch screen's mode, whose table is settings.h's g_backend_ini) to the
   backend, the on-screen controls and the unattended options: 1 when one of them took it */
int backend_ini(const char* k, const char* v);
/* the touch screen's mode chosen in the game's menu, saved to the .ini: 0 off, 1 fingers, 2 fingers and the mouse */
void backend_touch_set(int mode);

/* The window in pixels, for the swapchain; 0 x 0 when headless */
void backend_window_size(int* w, int* h);
/* The part of the window not under system bars or a camera cutout, in pixels (the whole window when fullscreen) */
void backend_window_safe(int* x, int* y, int* w, int* h);
/* 0 while nothing presented can be seen: the window minimized or covered, or its surface taken away (Android in the
   background) */
int backend_window_shown(void);
/* For a screen before the game starts that a press skips: what was pressed since the last call ("the key Return"),
   NULL for nothing. The presses that count are a key that types, an arrow or the keypad (not Esc, the function keys,
   modifiers, or a key with Ctrl, Alt or the system key), a finger on the screen, a pad's button but Home, or ZL or
   ZR (not one held as the pad came). From the first call until the frontend's first pass once the game's first
   thread runs (frontend_frame), they are that screen's alone and don't reach the game (a pad's buttons held then do
   once let go), unless the menu is over it, which takes them as over the game. The text lasts until the next call;
   main thread */
const char* backend_pressed(void);
/* the folder to find the launcher's files from, as snx_menu_dir takes it (malloc'd, NULL out of memory): the game's
   own folder (g_exedir), which snx_menu_dir walks up from to the data folder; on Android the data folder in the
   app's files (src/gui/android.c), wherever the game is */
char* backend_launcher_folder(void);
/* changes whenever the surface comes back, so the renderer knows to make a new Vulkan surface */
u32 backend_window_generation(void);
/* The software keyboard's box over the game (overlay.h), from any thread: 0 shown, in place of one that shows, or -1
   with no window to show it in (headless). The main thread turns the window's text input on while it shows, and hands
   its answer to am_keyboard_answered (services.h). Hide takes the box of that ticket away unanswered */
int backend_keyboard_show(const OverlayKeyboard* k);
void backend_keyboard_hide(u32 ticket);

/* fatal's message box. Called on every platform and from any thread; does nothing where there is no box */
void win_fatal_box(const char* msg);

/* Vulkan, unused when headless. The two handle types are declared here as vulkan_core.h declares them for 64-bit
   hosts (a repeated identical typedef is valid), so callers need no Vulkan header */
typedef struct VkInstance_T* VkInstance;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
void* backend_vk_loader(void); /* the address of vkGetInstanceProcAddr */
/* the Vulkan driver the program's own folders carry, where there is one (an Apple app's MoltenVK, or the copy beside a
   game's runtime): the address of vkGetInstanceProcAddr, NULL where the system's loader serves. A headless run loads
   it here, as a window's run has SDL load it */
void* backend_vk_driver(void);
const char* const* backend_vk_extensions(u32* n); /* the instance extensions the window needs */
int backend_vk_surface(VkInstance instance, VkSurfaceKHR* surface);

#endif
