// SPDX-License-Identifier: GPL-2.0-or-later
// MiSTer hybrid core support, see mister_port.h

#ifdef MISTER_HYBRID

#include "duke3d.h"
#include "mister.h"
#include "mister_port.h"
#include "cheats.h"
#include "osdcmds.h"
#include "common_game.h"

#include <unistd.h>

#define EXIT_TO_MENU 0
#define EXIT_RESTART 42 // danik_hybrid_launch.sh starts the game again
#define EXIT_RELOADED 43 // the same, after the core has settled

// OSD options of the game: CONF_STR in core/EDuke32.sv, status bits 24..63.
// The first entry of each is 0, the default.
#define OPT_RESOLUTION 24 // 2 bits
#define OPT_LSTICK     26 // 3 bits
#define OPT_RSTICK     29 // 3 bits
#define OPT_LIMITLOOK  32 // On is 0
#define OPT_RUMBLE     33 // 2 bits: On, Strong, Off
#define OPT_GOD        35 // the Cheats page
#define OPT_NOCLIP     36
#define OPT_SHOWMAP    37
#define OPT_NOMONSTERS 38
// Triggers T[56..59], bits of MH_OSDTriggers()
#define TRIG_EVERYTHING 1
#define TRIG_WEAPONS    2
#define TRIG_KEYS       4
#define TRIG_NEXTLEVEL  8

static int OSDOption(int lsb, int width)
{
    return MH_OSD_GAME_BITS(MH_OSDStatus(), lsb, width);
}

// Resolution: the video mode of the core. One the core does not have (an
// older core) is the one of 200 lines, which every core has; 320x200 as well
// when there is no core to ask.
static int ScreenMode(void)
{
    static const int option[4] = { MH_MODE_320x200, MH_MODE_640x200, MH_MODE_320x240, MH_MODE_640x240 };
    int mode;

    if (!MH_IsOpen())
        return MH_MODE_320x200;
    mode = option[OSDOption(OPT_RESOLUTION, 2)];
    if (!MH_ModeAvailable(mode))
        mode = MH_ModeWidth(mode) == 640 ? MH_MODE_640x200 : MH_MODE_320x200;
    return mode;
}

void MiSTer_ScreenSize(int32_t *x, int32_t *y)
{
    *x = MH_ModeWidth(ScreenMode());
    *y = MH_ModeHeight(ScreenMode());
}

// The HUD at 240 lines: the 200 lines of 2D art are in the middle, 20 lines
// from each edge; the HUD goes to the player's margins from there
static int HudMove(int edge)
{
    int top, bottom;

    if (ydim <= 200)
        return 0;
    MH_HudMargins(&top, &bottom);
    return (ydim - 200) / 2 - (edge ? bottom : top) * ydim / 240;
}

int MiSTer_HudUp(void)
{
    return HudMove(0);
}

int MiSTer_HudDown(void)
{
    return HudMove(1);
}

int MiSTer_HudBottom(void)
{
    int top, bottom;

    if (ydim <= 200)
        return 0;
    MH_HudMargins(&top, &bottom);
    return bottom * ydim / 240;
}

int MiSTer_StatusBarOverlay(void)
{
    return ud.statusbarmode || (MH_IsOpen() && ydim > 200);
}

int MiSTer_Rumble(void)
{
    static const int rumble[4] = { 1, 2, 0, 0 };

    return MH_IsOpen() ? rumble[OSDOption(OPT_RUMBLE, 2)] : 1;
}

int MiSTer_StickSensitivity(int stick)
{
    static const int percent[8] = { 100, 125, 150, 200, 300, 25, 50, 75 };

    return percent[OSDOption(stick ? OPT_RSTICK : OPT_LSTICK, 3)];
}

int MiSTer_LimitLook(void)
{
    // On (the default) is the bit clear
    return MH_IsOpen() && !OSDOption(OPT_LIMITLOOK, 1);
}

// The resolution, when the player changed it in the OSD
static void UpdateResolution(void)
{
    int32_t x, y;

    MiSTer_ScreenSize(&x, &y);
    // the HUD lines of the OSD only do something at 240 lines (D0)
    MH_SetMenuMask(y > 200 ? 0 : 1);
    if (!in3dmode() || (x == xdim && y == ydim))
        return;
    if (videoSetGameMode(ud.setup.fullscreen, x, y, 8, 1) < 0)
        return;
    onvideomodechange(0);
    g_restorePalette = -1;
    G_UpdateScreenArea();
    ud.setup.xdim = x;
    ud.setup.ydim = y;
}

// The game's own cheat, as from its console: its message, and refused at
// Damn I'm Good as the game refuses it. One a frame.
static int Cheat(int cheat)
{
    if (osdcmd_cheatsinfo_stat.cheatnum != -1)
        return 0;
    osdcmd_cheatsinfo_stat.cheatnum = cheat;
    return 1;
}

// An option of the Cheats page: on, it keeps the cheat on (the game turns
// God Mode off when a demo plays, Show Full Map with every level); turned
// off, it turns it off. A cheat typed in stays while the option is off.
struct CheatOption
{
    int bit, cheat, last;
};
static CheatOption CheatOptions[] = {
    { OPT_GOD, CHEAT_CORNHOLIO, 0 },
    { OPT_NOCLIP, CHEAT_CLIP, 0 },
    { OPT_SHOWMAP, CHEAT_SHOWMAP, 0 },
    { OPT_NOMONSTERS, CHEAT_MONSTERS, 0 },
};

static int CheatOn(int cheat)
{
    switch (cheat)
    {
        case CHEAT_CORNHOLIO: return ud.god;
        case CHEAT_CLIP: return ud.noclip;
        case CHEAT_SHOWMAP: return ud.showallmap;
        default: return g_noEnemies == 1;
    }
}

// Next Level: as the level's exit; after the last level of an episode, its end
static void NextLevel(DukePlayer_t *pPlayer)
{
    int const next = ud.volume_number * MAXLEVELS + ud.level_number + 1;

    pPlayer->player_par = 0; // no best time
    if (ud.from_bonus || (ud.level_number + 1 < MAXLEVELS && g_mapInfo[next].filename != NULL))
        P_EndLevel();
    else
    {
        ud.eog = 1;
        pPlayer->gm |= MODE_EOL;
    }
}

static void UpdateCheats(void)
{
    auto const pPlayer = g_player[myconnectindex].ps;
    int const triggers = MH_OSDTriggers();
    int const playing = (pPlayer->gm & MODE_GAME) && !(pPlayer->gm & (MODE_DEMO | MODE_EOL | MODE_RESTART))
                        && numplayers < 2 && ud.recstat == 0 && !G_HaveUserMap();

    for (auto &option : CheatOptions)
    {
        int const on = OSDOption(option.bit, 1);
        int const changed = on != option.last;

        if (!playing)
            continue; // turned on in the title, it applies once the game runs
        option.last = on;
        if (on == CheatOn(option.cheat) || (!on && !changed))
            continue;
        if (!FURY && ud.player_skill == 4)
        {
            // none at Damn I'm Good
            if (changed)
                P_DoQuote(QUOTE_CHEATS_DISABLED, pPlayer);
            continue;
        }
        if (option.cheat == CHEAT_MONSTERS)
        {
            // No Monsters is the game's "Monsters: Off", not its cycle
            g_noEnemies = on;
            Bsprintf(apStrings[QUOTE_RESERVED4], "Monsters: %s", on ? "Off" : "On");
            P_DoQuote(QUOTE_RESERVED4, pPlayer);
        }
        else if (!changed && option.cheat == CHEAT_SHOWMAP)
        {
            // again with every level: no message
            ud.showallmap = 1;
            for (char &i : show2dsector)
                i = 255;
        }
        else if (!Cheat(option.cheat))
            option.last = !on; // the next frame
    }

    if (!playing)
        return;
    if (triggers & TRIG_EVERYTHING)
        Cheat(CHEAT_STUFF);
    else if (triggers & TRIG_WEAPONS)
        Cheat(CHEAT_WEAPONS);
    else if (triggers & TRIG_KEYS)
        Cheat(CHEAT_KEYS);
    if (triggers & TRIG_NEXTLEVEL)
    {
        if (!FURY && ud.player_skill == 4)
            P_DoQuote(QUOTE_CHEATS_DISABLED, pPlayer);
        else
            NextLevel(pPlayer);
    }
}

// The sticks' sensitivity: the game's own setting of each axis times the OSD's
static void UpdateSticks(void)
{
    for (int axis = 0; axis < 4; axis++)
        CONTROL_SetAnalogAxisSensitivity(axis, ud.config.JoystickAnalogueSensitivity[axis] *
                                         MiSTer_StickSensitivity(axis >= 2) / 100.f, controldevice_joystick);
}

void MiSTer_UpdateOptions(void)
{
    if (!MH_IsOpen())
        return;
    UpdateResolution();
    UpdateSticks();
    UpdateCheats();
    // the d-pad Main_MiSTer presses with the left stick moves the menus, and
    // not the player, whom the stick moves itself
    int const gm = g_player[myconnectindex].ps->gm;
    misterStickDpad = (gm & MODE_MENU) || !(gm & MODE_GAME) || (gm & (MODE_DEMO | MODE_EOL));
}

// The player came through the list: quitting a game goes back to it
static int PickedFromList = 0;
static char SaveDir[BMAX_PATH];

// The games with a menu entry of their own (package/games.txt) and the
// shareware that Duke Nukem 3D's entry starts when the full game is not
// there: the game's own kinds of GRP file (grpscan.cpp), which it knows by
// their checksum, whatever their name, each the first of its list that was
// found. The add-ons need the full game's GRP of their version as well: the
// game drops those whose full game it did not find.
struct MiSTerGame
{
    const char *id;
    const char *name;
    const char *shareware_of;
    const char *missing;
    int32_t crc[8];
};
static const MiSTerGame Games[] = {
    { "DN3D", "Duke Nukem 3D", NULL,
      "Duke Nukem 3D was not found. Copy DUKE3D.GRP (version 1.3D, 1.4, 1.5 Atomic "
      "Edition or the shareware) to %s/.",
      { DUKE15_CRC, DUKEPP_CRC, DUKEWT_CRC, DUKE13_CRC, DUKEKR_CRC } },
    { "DN3DS", "Duke Nukem 3D Shareware", "DN3D", NULL,
      { DUKESW_CRC, DUKE11_CRC, DUKE10_CRC, DUKE099_CRC, DUKEMD_CRC, DUKEMD2_CRC } },
    { "DC", "DN3D: Duke It Out in D.C.", NULL,
      "Duke It Out in D.C. was not found. It needs DUKEDC.GRP and DUKE3D.GRP of the "
      "full game, of the same version (1.3D or Atomic): copy both to %s/.",
      { DUKEDC_CRC, DUKEDCPP_CRC, (int32_t)0x39A692BF, (int32_t)0xC63B6A8B, DUKEDC13_CRC } },
    { "VACA", "DN3D: Life's a Beach", NULL,
      "Life's a Beach was not found. It needs VACATION.GRP and DUKE3D.GRP of the full "
      "game, of the same version (1.3D or Atomic): copy both to %s/.",
      { VACA15_CRC, DUKECB_CRC, VACAPP_CRC, (int32_t)0x65B5F690, (int32_t)0x64CF2351, VACA13_CRC } },
    { "NW", "DN3D: Nuclear Winter", NULL,
      "Nuclear Winter was not found. It needs NWINTER.GRP and DUKE3D.GRP of the Atomic "
      "Edition (1.4 or 1.5): copy both to %s/.",
      { DUKENW_CRC, (int32_t)0xC7EFBFA9 } },
    { "DZ2", "DN3D: Duke!ZONE II", NULL,
      "Duke!ZONE II was not found. It needs its GRP file (DUKEZONE2.GRP of Raze's "
      "release, or the one of its CD) and DUKE3D.GRP of the full game: copy both to %s/.",
      { (int32_t)0x3B29D5C7, DZ2_PP_CRC, (int32_t)0x1E9516F1, DZ2_13_CRC } },
    { "PP", "DN3D: Penthouse Paradise", NULL,
      "Penthouse Paradise was not found. It needs PENTHOUS.ZIP (or the GRP of ZOOM "
      "Platform's release) and DUKE3D.GRP of the Atomic Edition (1.4 or 1.5): copy both "
      "to %s/.",
      { (int32_t)0x7CD82A3B, (int32_t)0xCF928A58 } },
};
#define NUM_GAMES ARRAY_SSIZE(Games)
#define NO_GAME "No game data found. Copy DUKE3D.GRP of Duke Nukem 3D (the full game or " \
    "the shareware) to %s/, and the add-ons' files (DUKEDC.GRP, VACATION.GRP, NWINTER.GRP, " \
    "DUKEZONE2.GRP, PENTHOUS.ZIP) next to it."

#define MAX_GAMES 16

grpfile_t const *MiSTer_SelectGroup(void)
{
    mh_game games[MAX_GAMES];
    grpfile_t const *of[MAX_GAMES];
    static const char *files[MAX_GAMES][2];
    static const char *const none[] = { NULL };
    int n = 0;

    if (!MH_Open())
        return g_selectedGrp; // no core: the game's own choice

    // the games of the menu entries, found or not (never there then, whatever
    // file names are in the folder: the shareware's GRP is called DUKE3D.GRP
    // too)
    for (auto const &g : Games)
    {
        grpfile_t const *grp = NULL;

        for (int i = 0; i < 8 && g.crc[i] && !grp; i++)
            grp = FindGroup(g.crc[i]);
        if (!grp && !g.missing)
            continue;
        mh_game game = { g.id, g.name, MH_NOT_THERE, g.shareware_of, g.missing };
        if (grp)
        {
            // its GRP file for the lines under the logo ("Checking Game Data..."),
            // in the game's folder or where Megaton Edition has its add-ons
            // (G_AddMiSTerSearchPaths())
            static char const *const dirs[] = { "", "addons/dc/", "addons/nw/", "addons/vacation/" };
            static char paths[MAX_GAMES][BMAX_PATH];
            game.files = none;
            for (char const *dir : dirs)
            {
                Bsnprintf(paths[n], sizeof(paths[n]), "%s%s", dir, grp->filename);
                if (access(paths[n], F_OK) == 0)
                {
                    files[n][0] = paths[n];
                    files[n][1] = NULL;
                    game.files = files[n];
                    break;
                }
            }
        }
        of[n] = grp;
        games[n++] = game;
    }
    int const entries = n;
    // the other add-ons of Duke Nukem 3D the game knows (one a .grpinfo file
    // describes), for the list only
    for (grpfile_t const *grp = foundgrps; grp && n < MAX_GAMES; grp = grp->next)
    {
        int known = 0;

        if ((grp->type->game & (GAMEFLAG_DUKE | GAMEFLAG_ADDON)) != (GAMEFLAG_DUKE | GAMEFLAG_ADDON))
            continue;
        for (auto const &g : Games)
            for (int i = 0; i < 8 && g.crc[i]; i++)
                known |= g.crc[i] == grp->type->crcval;
        for (int i = 0; i < n; i++)
            known |= of[i] && !Bstrcmp(games[i].name, grp->type->name);
        if (known)
            continue;
        mh_game game = { grp->type->name, grp->type->name, none, NULL, NULL };
        of[n] = grp;
        games[n++] = game;
    }

    int const pick = MH_PickGame(games, n, NULL, NO_GAME, &PickedFromList);
    if (pick < 0 || of[pick] == NULL)
        exit(0);
    // the add-ons' saved games in a folder of their own: one by its id, one
    // of the list by its GRP's name
    if (pick >= entries || (Bstrcmp(games[pick].id, "DN3D") && Bstrcmp(games[pick].id, "DN3DS")))
    {
        char const *name = pick < entries ? games[pick].id : of[pick]->filename;
        char const *slash = Bstrrchr(name, '/');
        Bsnprintf(SaveDir, sizeof(SaveDir), "%s", slash ? slash + 1 : name);
        if (char *dot = Bstrrchr(SaveDir, '.'))
            *dot = 0;
        Bstrcat(SaveDir, "/");
    }
    // the game's logo and the lines under it ("Loading <name>..." the last)
    // come with the pick (MH_GameLogo(), host version 17)
    return of[pick];
}

char const *MiSTer_SaveDir(void)
{
    return SaveDir;
}

// Runs after everything else of exit(), whichever of the game's ways out was
// taken
static void ExitWithCode(void)
{
    _exit(MiSTer_ExitCode());
}

void MiSTer_Init(void)
{
    atexit(ExitWithCode);
}

int MiSTer_ExitCode(void)
{
    // The player loaded the core again while the game ran
    if (MH_CoreReloaded())
        return EXIT_RELOADED;
    // Not if we are quitting because another core was loaded
    if (PickedFromList && MH_Open())
        return EXIT_RESTART;
    return EXIT_TO_MENU;
}

#endif
