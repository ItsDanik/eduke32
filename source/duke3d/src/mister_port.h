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
void MiSTer_VfsBench(void);

// A level's map is loaded, its sprites not yet set up: mends what is wrong in
// the map itself (premap.cpp)
void MiSTer_FixMap(void);

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

// The 3D view drawn on the second CPU while the game moves on to the next
// frame (mister_port.cpp, "The renderer's thread"). 1 when it is on
// (MISTER_HYBRID_RENDERTHREAD=0 turns it off) and the core is there.
int MiSTer_RenderThreaded(void);
// Draw the view (G_DrawRoomsView()) with the camera as it is set (ud.camera*)
void MiSTer_RenderKick(int32_t smoothRatio);
// Until it is drawn; the sprite animations it asks for meanwhile are run
void MiSTer_RenderWait(void);
// Often, in the game's loops: the sprite animation the renderer may wait for
void MiSTer_RenderPoll(void);
// Before the world changes under the renderer (a level, a saved game, the
// video mode, the exit): the view in flight is finished (G_MiSTerViewFinish())
void MiSTer_RenderSync(void);
// 1 on the renderer's thread
int MiSTer_OnRenderThread(void);
// 1 while a view is being drawn there
int MiSTer_RenderBusy(void);
// From the renderer's thread: fn(arg) run by the game's thread, which alone
// may run the scripts; the renderer waits for it
void MiSTer_RenderCall(void (*fn)(void *), void *arg);
// G_DoSpriteAnimations() that way
void MiSTer_RenderAnimateSprites(int32_t x, int32_t y, int32_t z, int32_t a, int32_t smoothratio);
// game.cpp: the 3D view of G_DrawRooms() once the camera is set; whether one
// is being drawn on the other CPU, and finishing it (the rest of G_DrawRooms())
void G_DrawRoomsView(int32_t smoothRatio);
int G_MiSTerViewPending(void);
void G_MiSTerViewFinish(void);
#define MISTER_RENDER_POLL() MiSTer_RenderPoll()

// Debugging: what each thread was doing, in /tmp/fury_trace.bin, which
// survives the game's death (od -An -tu4 /tmp/fury_trace.bin)
struct MiSTerTrace
{
    uint32_t mainFrame, mainPhase, mainWorldPhase, mainActor, mainRpcDepth;
    uint32_t renderFrame, renderPhase, renderYaxLevel;
    int32_t  rpcArgs[5];
    uint32_t renderPending, renderBusy, renderWanted;
};
extern MiSTerTrace *misterTrace;
#define MISTER_TRACE(field, value) do { if (misterTrace) misterTrace->field = (value); } while (0)

// The screen as the scripts see it (gamevars.cpp: xdim, windowx1, windowx2):
// 320 wide in the 640-wide modes, which the core shows 4:3 like the others,
// so that scripts laying the HUD out for the aspect ratio do it for 4:3.
// The pixel x coordinates they pass to the drawing commands are scaled back.
extern int32_t misterConXdim, misterConWindowX1, misterConWindowX2, misterConXScale;
extern int32_t misterConYdim, misterConWindowY1, misterConWindowY2;
void MiSTer_UpdateConScreen(void);
#define MISTER_CON_XDIM misterConXdim
#define MISTER_CON_YDIM misterConYdim
#define MISTER_CON_WINDOWX1 misterConWindowX1
#define MISTER_CON_WINDOWX2 misterConWindowX2
#define MISTER_CON_WINDOWY1 misterConWindowY1
#define MISTER_CON_WINDOWY2 misterConWindowY2
#define MISTER_CON_X1(x) ((x) * misterConXScale)
#define MISTER_CON_X2(x) (((x) + 1) * misterConXScale - 1)
#define MISTER_CON_Y1(y) ((y) * ydim / misterConYdim)
#define MISTER_CON_Y2(y) (((y) + 1) * ydim / misterConYdim - 1)

// The 2D art drawn in `stmt` (the HUD) moved down the screen by `lines`
#define MISTER_HUD(lines, stmt) do { rotatesprite_mister_yofs = (lines); stmt; rotatesprite_mister_yofs = 0; } while (0)

#else

#define MISTER_HUD(lines, stmt) do { stmt; } while (0)
#define MISTER_RENDER_POLL() ((void)0)
#define MISTER_CON_XDIM xdim
#define MISTER_CON_YDIM ydim
#define MISTER_CON_WINDOWX1 windowxy1.x
#define MISTER_CON_WINDOWX2 windowxy2.x
#define MISTER_CON_WINDOWY1 windowxy1.y
#define MISTER_CON_WINDOWY2 windowxy2.y
#define MISTER_CON_X1(x) (x)
#define MISTER_CON_X2(x) (x)
#define MISTER_CON_Y1(y) (y)
#define MISTER_CON_Y2(y) (y)
#define MISTER_TRACE(field, value) ((void)0)

#endif
