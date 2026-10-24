// SPDX-License-Identifier: GPL-2.0-or-later
// MiSTer hybrid core (github.com/ItsDanik/EDuke32_MiSTer): what is specific
// to the game. The engine's part is in source/build (mister.h, MISTER_HYBRID
// in sdlayer.cpp, engine.cpp, timer.cpp). Everything here does nothing
// without the core, so the PC build of the same code still runs.

#pragma once

#ifdef MISTER_HYBRID

#include "grpscan.h"
#include "mister.h"

// At the start, before anything else: the core, and the exit code of every
// way out of the game (danik_hybrid_launch.sh)
void MiSTer_Init(void);
// Which game to start, after G_ScanGroups(): from the menu entry (MH_PickGame(),
// the GRP files the game knows by their checksum) or from the list. Shows
// the message when its files are not there and does not come back then.
grpfile_t const *MiSTer_SelectGroup(void);

// Where the game's saved games are, "" or a folder ending in "/": the
// add-ons' are each in a folder of their own, so that no game lists, or
// loads, another's (savegame.cpp)
char const *MiSTer_SaveDir(void);

// The video mode the OSD's Resolution says: the size of the game's screen
void MiSTer_ScreenSize(int32_t *x, int32_t *y);
// Once per frame, in the game's loops: the OSD options that apply while the
// game runs (the resolution, the HUD lines, the cheats)
void MiSTer_UpdateOptions(void);

// The HUD at 240 lines (hybrid/README.md, "HUD margins"): how many lines the
// HUD at the top is moved up from where it is in the 200 lines in the middle
// of the screen, and the one at the bottom down. 0 at 200 lines.
int MiSTer_HudUp(void);
int MiSTer_HudDown(void);
// The bottom margin in lines: what the weapon, at the bottom of the view, is
// moved up by. 0 at 200 lines.
int MiSTer_HudBottom(void);
// 1 if the status bar is drawn over the view: at 240 lines, which the view
// fills, whatever the game's setting
int MiSTer_StatusBarOverlay(void);

// Rumble from the OSD: 0 off, 1 as the game has it, 2 stronger
int MiSTer_Rumble(void);
// The sensitivity of the left (0) and right (1) stick, in percent
int MiSTer_StickSensitivity(int stick);
// Limit Vertical Look: 1 to look up and down half as far from the middle
int MiSTer_LimitLook(void);

// The game's exit code: the launcher starts it again for the list it was
// picked from, or after the core was loaded again
int MiSTer_ExitCode(void);

// The 2D art drawn in `stmt` (the HUD) moved down the screen by `lines`
#define MISTER_HUD(lines, stmt) do { rotatesprite_mister_yofs = (lines); stmt; rotatesprite_mister_yofs = 0; } while (0)

#else

#define MISTER_HUD(lines, stmt) do { stmt; } while (0)

#endif
