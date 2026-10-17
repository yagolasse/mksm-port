/*
 * The in-game settings menu (plan 4.6, B7a1): Dear ImGui over the GPU window present.
 * Plain-C interface for main.c.
 */
#ifndef MKSM_MENU_H
#define MKSM_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

/* Hooks the menu into the window's events (F1 / Esc), the pads (Back+Start, a neutral pad for
 * the game while open) and the GPU present (draws it). Only for the SDL window with the GPU
 * window present; call before the window opens. */
void mksm_menu_install(void);

#ifdef __cplusplus
}
#endif

#endif
