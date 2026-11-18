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
    // development: MISTER_HYBRID_RESOLUTION=640x200 etc. instead of the OSD
    if (char const *env = getenv("MISTER_HYBRID_RESOLUTION"))
    {
        int w = 0, h = 0;
        if (sscanf(env, "%dx%d", &w, &h) == 2)
            mode = h == 240 ? (w == 640 ? MH_MODE_640x240 : MH_MODE_320x240) : (w == 640 ? MH_MODE_640x200 : MH_MODE_320x200);
        else
            mode = option[OSDOption(OPT_RESOLUTION, 2)];
    }
    else
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
    MiSTer_RenderSync();
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
// found, or one of a version it does not know by the name its own GRPINFO
// gives it. The add-ons need the full game's GRP of their version as well:
// the game drops those whose full game it did not find.
struct MiSTerGame
{
    const char *id;
    const char *name;
    const char *shareware_of;
    const char *missing;
    int32_t crc[8];
    const char *grpname;
};
#ifdef EDUKE32_STANDALONE
// Ion Fury (the FURY=1 build): one game, known by its fury.grpinfo next to
// fury.grp (GAMEFLAG_FURY), not by a checksum the port lists, so every
// version of the game and Aftershock are taken as they are. Its menu entry
// loads the EDuke32 core with the set name EDuke32_FURY.
static const MiSTerGame Games[] = {
    { "FURY", "Ion Fury", NULL,
      "Ion Fury was not found. Copy fury.grp, fury.grpinfo and fury.def from the "
      "game's folder to %s/.",
      { 0 } },
};
#define NUM_GAMES ARRAY_SSIZE(Games)
#define NO_GAME "No game data found. Copy fury.grp, fury.grpinfo and fury.def from " \
    "Ion Fury's folder to %s/."
#else
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
      "Duke!ZONE II was not found. Copy NightFright's dukezone2 folder (or its "
      "DUKEZONE2.GRP, or the GRP of the CD) to %s/, with DUKE3D.GRP of the full game.",
      { (int32_t)0x9BA30F03, (int32_t)0x3B29D5C7, DZ2_PP_CRC, (int32_t)0x1E9516F1, DZ2_13_CRC },
      "Duke!Zone II" },
    { "PP", "DN3D: Penthouse Paradise", NULL,
      "Penthouse Paradise was not found. It needs PENTHOUS.ZIP (or the GRP of ZOOM "
      "Platform's release) and DUKE3D.GRP of the Atomic Edition (1.4 or 1.5): copy both "
      "to %s/.",
      { (int32_t)0x7CD82A3B, (int32_t)0xCF928A58 } },
    // the other games of the Duke Nukem 3D engine EDuke32 plays, each in a
    // folder of its own (games/dnk/EDuke32_<id>/, with a launch script that
    // runs the game there)
    { "NAM", "NAM", NULL,
      "NAM was not found. Copy NAM.GRP, NAM.CON and NAM.RTS from the game's folder "
      "to %s/.",
      { NAM_CRC, NAPALM_CRC } },
    { "WW2GI", "WWII GI", NULL,
      "World War II GI was not found. Copy WW2GI.GRP and WW2GI.RTS from the game's "
      "folder to %s/.",
      { WW2GI_CRC } },
    { "PLATOONL", "WWII GI: Platoon Leader", NULL,
      "Platoon Leader was not found. It needs PLATOONL.DAT and PLATOONL.DEF with "
      "WW2GI.GRP of World War II GI: copy them to %s/.",
      { PLATOONL_CRC } },
    // Zombie Crisis (Childs/van Dijk, 2008): a stand-alone total conversion
    // whose 7 MB DUKE3D.GRP has everything, described by zombiecrisis.grpinfo
    { "ZOMBIECRISIS", "Zombie Crisis", NULL,
      "Zombie Crisis was not found. Copy DUKE3D.GRP from the game's folder to %s/, "
      "with the zombiecrisis.grpinfo of the add-on.",
      { (int32_t)0x5CA58B1F } },
    // A.W.O.L. (Shotspark Studios, 2024): its awol.grp (a zip) with the
    // add-on's awol.grpinfo next to it (scriptname, defname, the game flags)
    { "AWOL", "A.W.O.L.", NULL,
      "A.W.O.L. was not found. Copy awol.grp from the game's folder to %s/, with the "
      "awol.grpinfo of the add-on.",
      { (int32_t)0xDA99945F } },
};
#define NUM_GAMES ARRAY_SSIZE(Games)
#define NO_GAME "No game data found. Copy DUKE3D.GRP of Duke Nukem 3D (the full game or " \
    "the shareware) to %s/, and the add-ons' files (DUKEDC.GRP, VACATION.GRP, NWINTER.GRP, " \
    "DUKEZONE2.GRP, PENTHOUS.ZIP) next to it. NAM (NAM.GRP) and World War II GI " \
    "(WW2GI.GRP) go into games/dnk/EDuke32_NAM/ and games/dnk/EDuke32_WW2GI/."

#endif

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
        for (grpfile_t const *f = foundgrps; f && !grp && g.grpname; f = f->next)
            if ((f->type->game & GAMEFLAG_ADDON) && !Bstrcasecmp(f->type->name, g.grpname))
                grp = f;
#ifdef EDUKE32_STANDALONE
        // Ion Fury: the first GRP its GRPINFO says is the game itself
        for (grpfile_t const *f = foundgrps; f && !grp; f = f->next)
            if ((f->type->game & GAMEFLAG_FURY) && !(f->type->game & GAMEFLAG_ADDON))
                grp = f;
        for (grpfile_t const *f = foundgrps; f && !grp; f = f->next)
            if (f->type->game & GAMEFLAG_FURY)
                grp = f;
#endif
        if (!grp && !g.missing)
            continue;
        mh_game game = { g.id, g.name, MH_NOT_THERE, g.shareware_of, g.missing };
        if (grp)
        {
            // its GRP file for the lines under the logo ("Checking Game Data..."),
            // in the game's folder or where Megaton Edition has its add-ons
            // (G_AddMiSTerSearchPaths())
            static char const *const dirs[] = { "", "addons/dc/", "addons/nw/", "addons/vacation/", "dukezone2/" };
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
        {
            for (int i = 0; i < 8 && g.crc[i]; i++)
                known |= g.crc[i] == grp->type->crcval;
            known |= g.grpname && !Bstrcasecmp(g.grpname, grp->type->name);
        }
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
    if (pick >= entries || (Bstrcmp(games[pick].id, "DN3D") && Bstrcmp(games[pick].id, "DN3DS") && Bstrcmp(games[pick].id, "FURY")))
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

// MISTER_HYBRID_VFSBENCH=<rounds> in the environment (development): once the
// game's data files are mounted, opens and closes a list of files that many
// times, prints what an open took and leaves. The names: a line each in
// vfsbench.txt in the game's folder, else the art files and palette every
// game has. With MISTER_HYBRID_PATHCACHE=0 it is the time without the file
// lookup cache (source/build/include/pathcache.h), to compare on the MiSTer's
// own SD card.
void MiSTer_VfsBench(void)
{
    char const *env = getenv("MISTER_HYBRID_VFSBENCH");
    int const rounds = env ? atoi(env) : 0;
    static char names[256][96];
    int n = 0;

    if (rounds <= 0)
        return;
    if (FILE *f = fopen("vfsbench.txt", "r"))
    {
        while (n < 256 && fgets(names[n], sizeof(names[n]), f))
        {
            names[n][strcspn(names[n], "\r\n")] = 0;
            n += names[n][0] != 0;
        }
        fclose(f);
    }
    if (n == 0)
    {
        for (; n < 20; n++)
            Bsnprintf(names[n], sizeof(names[n]), "tiles%03d.art", n);
        Bstrcpy(names[n++], "palette.dat");
        Bstrcpy(names[n++], "lookup.dat");
    }

    int found = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < rounds; r++)
        for (int i = 0; i < n; i++)
        {
            buildvfs_kfd const fil = kopen4loadfrommod(names[i], 0);
            if (fil != buildvfs_kfd_invalid)
            {
                found++;
                kclose(fil);
            }
        }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double const us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
    LOG_F(INFO, "VFS benchmark: %d opens of %d names (%d found), %.1f us each, %.1f ms in all.",
          rounds * n, n, found, us / (rounds * n), us / 1000);
    vfs_pathcache_report();
    exit(0);
}

char const *MiSTer_SaveDir(void)
{
    return SaveDir;
}

void MiSTer_FixMap(void)
{
    // Penthouse Paradise, ppakpent.map: a third transporter on each of the two
    // pairs that join the ocean's surface and its water, which PENTHOUS.MAP
    // (the level for 1.3D) does not have. A transporter goes to the first
    // sprite of its hitag, so these took the player who swam up back to the
    // bottom, or out of the map from the deep water around them.
    static const struct { int16_t spriteNum, hitag; vec3_t pos; } Strays[] = {
        { 1, 39, { 36480, 25728, 8192 } },
        { 2, 40, { 35200, 25728, 8192 } },
    };

    for (auto const &stray : Strays)
    {
        auto const &s = sprite[stray.spriteNum];

        if (s.statnum < MAXSTATUS && s.picnum == SECTOREFFECTOR && s.lotag == SE_7_TELEPORT && s.hitag == stray.hitag
            && s.x == stray.pos.x && s.y == stray.pos.y && s.z == stray.pos.z)
            deletesprite(stray.spriteNum);
    }
}

// ---- The screen as the scripts see it (mister_port.h) ----
int32_t misterConXdim = 320, misterConWindowX1, misterConWindowX2 = 319, misterConXScale = 1;
int32_t misterConYdim = 240, misterConWindowY1, misterConWindowY2 = 239;

void MiSTer_UpdateConScreen(void)
{
    misterConXScale   = (MH_IsOpen() && xdim >= 640 && ydim <= 240) ? xdim / 320 : 1;
    misterConXdim     = xdim / misterConXScale;
    misterConWindowX1 = windowxy1.x / misterConXScale;
    misterConWindowX2 = (windowxy2.x + 1) / misterConXScale - 1;
    // the 4:3 height for that width (240 for 320): the lines of a 200-line
    // mode are 1.2 pixels tall, and the layout the scripts make for 320x240
    // is squeezed to them by the engine (dorotspr_mister)
    misterConYdim     = MH_IsOpen() ? misterConXdim * 3 / 4 : ydim;
    misterConWindowY1 = windowxy1.y * misterConYdim / ydim;
    misterConWindowY2 = (windowxy2.y + 1) * misterConYdim / ydim - 1;

    static int32_t said[4];
    if (said[0] != xdim || said[1] != ydim || said[2] != misterConWindowX1 || said[3] != misterConWindowX2)
    {
        said[0] = xdim, said[1] = ydim, said[2] = misterConWindowX1, said[3] = misterConWindowX2;
        LOG_F(INFO, "mister: the screen is %dx%d, the scripts see %dx%d with the view at x %d..%d, y %d..%d", xdim, ydim,
              misterConXdim, misterConYdim, misterConWindowX1, misterConWindowX2, misterConWindowY1, misterConWindowY2);
    }
}

// ---- The renderer's thread ----
// The 3D view of a frame is drawn on the second CPU while the game's thread
// moves the world on for the next one (game.cpp: drawframe_do(),
// G_DrawRooms()); the view is shown with the next frame. The renderer reads
// the world as it changes (a sprite is drawn where it was a moment ago at
// worst, see classicDrawSprite()) and writes nothing of it; the game's thread
// draws nothing while a view is in flight. G_DoSpriteAnimations() and its
// scripts (EVENT_ANIMATESPRITES), which the renderer needs between the walls
// and the sprites of each layer, run on the game's thread: the renderer asks
// and waits, the game's thread serves the request when it polls
// (MiSTer_RenderPoll(): A_Execute(), G_MoveWorld()) or waits for the view.
// Files and the tile cache are shared, one thread at a time (misterIoLock()).
#include <pthread.h>

// A crash: which thread, and the return addresses on its stack (nm -nC of
// the binary tells the functions), in the log before the game dies as before
#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>

static pid_t crashGameTid;

static void CrashTrace(int sig)
{
    void *frames[48];
    int const n = backtrace(frames, ARRAY_SIZE(frames));
    char line[128];
    int len = snprintf(line, sizeof(line), "mister: signal %d on thread %ld (the game's thread is %ld), backtrace:\n", sig,
                       (long)syscall(SYS_gettid), (long)crashGameTid);
    write(1, line, len);
    for (int i = 0; i < n; i++)
    {
        len = snprintf(line, sizeof(line), "  %p\n", frames[i]);
        write(1, line, len);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

// this thread's own stack for the handler: a crash with a bad stack pointer
// (a stack overflow) could not run it otherwise
static void CrashTraceStack(void)
{
    stack_t ss = {};
    ss.ss_size = 1 << 16;
    ss.ss_sp = Xmalloc(ss.ss_size);
    sigaltstack(&ss, NULL);
}

static void CrashTraceInstall(void)
{
    static int const sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };

    crashGameTid = syscall(SYS_gettid);
    CrashTraceStack();
    for (int s : sigs)
    {
        struct sigaction sa = {};
        sa.sa_handler = CrashTrace;
        sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
        sigaction(s, &sa, NULL);
    }
}

#include <sys/mman.h>
#include <fcntl.h>

MiSTerTrace *misterTrace;

static void TraceOpen(void)
{
    int const fd = open("/tmp/fury_trace.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    if (ftruncate(fd, 4096) == 0)
    {
        void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED)
            misterTrace = (MiSTerTrace *)m;
    }
    close(fd);
}

static pthread_t renderThread;
static int renderThreadStarted;
static pthread_mutex_t renderLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t renderToGame = PTHREAD_COND_INITIALIZER; // the view is drawn, or an animation is wanted
static pthread_cond_t gameToRender = PTHREAD_COND_INITIALIZER; // a view to draw, or the animation is done
static int renderOn = -1;
static int renderBusy;             // a view is being drawn
static int32_t renderSmooth;
static volatile int renderWanted;  // the renderer waits for renderCallFn(renderCallArg)
static void (*renderCallFn)(void *);
static void *renderCallArg;
static int renderServed;

// The renderer's copy of the world (build.h): what it reads through sector,
// wall, sprite and the sector sprite lists on its thread. Filled by the
// game's thread before each view, while the renderer waits for it.
static struct
{
    sectortype *sector;
    walltype   *wall;
    spritetype *sprite;
    int16_t    *headspritesect, *prevspritesect, *nextspritesect;
} renderWorld;

static void RenderWorldAllocate(void)
{
    renderWorld.sector         = (sectortype *)Xcalloc(MAXSECTORS + M32_FIXME_SECTORS, sizeof(sectortype));
    renderWorld.wall           = (walltype *)Xcalloc(MAXWALLS + M32_FIXME_WALLS, sizeof(walltype));
    renderWorld.sprite         = (spritetype *)Xcalloc(MAXSPRITES, sizeof(spritetype));
    renderWorld.headspritesect = (int16_t *)Xcalloc(MAXSECTORS + 1, sizeof(int16_t));
    renderWorld.prevspritesect = (int16_t *)Xcalloc(MAXSPRITES, sizeof(int16_t));
    renderWorld.nextspritesect = (int16_t *)Xcalloc(MAXSPRITES, sizeof(int16_t));
}

// the game's thread, the renderer idle: the world of this moment
static void RenderWorldCopy(void)
{
    // sector, wall, sprite: the game's thread's, the engine's arrays
    Bmemcpy(renderWorld.sector, sector, sizeof(sectortype) * min<int>(numsectors + 1, MAXSECTORS + M32_FIXME_SECTORS));
    Bmemcpy(renderWorld.wall, wall, sizeof(walltype) * min<int>(numwalls + 1, MAXWALLS + M32_FIXME_WALLS));
    Bmemcpy(renderWorld.headspritesect, headspritesectStorage, sizeof(int16_t) * (MAXSECTORS + 1));
    Bmemcpy(renderWorld.prevspritesect, prevspritesectStorage, sizeof(int16_t) * MAXSPRITES);
    Bmemcpy(renderWorld.nextspritesect, nextspritesectStorage, sizeof(int16_t) * MAXSPRITES);
    // the sprites that are in a sector: the lists, from the game's thread's arrays
    for (int s = 0; s < numsectors; s++)
        for (int i = headspritesectStorage[s], n = 0; i >= 0 && n < MAXSPRITES; i = nextspritesectStorage[i], n++)
            renderWorld.sprite[i] = sprite[i];
}

int MiSTer_RenderThreaded(void)
{
    if (renderOn < 0)
    {
        char const *env = getenv("MISTER_HYBRID_RENDERTHREAD");
        renderOn = (env == NULL || atoi(env) != 0) && MH_IsOpen();
        LOG_F(INFO, "mister: the 3D view is drawn %s", renderOn ? "on the second CPU while the game moves on (MISTER_HYBRID_RENDERTHREAD=1)"
                                                                : "by the game's thread (MISTER_HYBRID_RENDERTHREAD=0)");
    }
    return renderOn;
}

int MiSTer_OnRenderThread(void)
{
    return renderThreadStarted && pthread_equal(pthread_self(), renderThread);
}

int MiSTer_RenderBusy(void)
{
    return __atomic_load_n(&renderBusy, __ATOMIC_ACQUIRE);
}

static void *RenderMain(void *)
{
    MH_PinThreadToCPU1();
    CrashTraceStack();
    // this thread reads the world from its copy
    sector         = renderWorld.sector;
    wall           = renderWorld.wall;
    sprite         = renderWorld.sprite;
    headspritesect = renderWorld.headspritesect;
    prevspritesect = renderWorld.prevspritesect;
    nextspritesect = renderWorld.nextspritesect;
    pthread_mutex_lock(&renderLock);
    for (;;)
    {
        while (!renderBusy)
            pthread_cond_wait(&gameToRender, &renderLock);
        int32_t const smooth = renderSmooth;
        pthread_mutex_unlock(&renderLock);
#ifdef MISTER_PROFILE
        uint64_t const t0 = misterProfNow();
#endif
        {
            static int views, crashAt = -1;
            if (crashAt < 0)
                crashAt = getenv("MISTER_HYBRID_CRASHTEST") ? atoi(getenv("MISTER_HYBRID_CRASHTEST")) : 0;
            MISTER_TRACE(renderFrame, ++views);
            MISTER_TRACE(renderPhase, 1);
            if (crashAt && views == crashAt)
                *(volatile int *)8 = views;
        }
        G_DrawRoomsView(smooth);
        MISTER_TRACE(renderPhase, 0);
#ifdef MISTER_PROFILE
        misterProfAdd(MPROF_RTHREAD, misterProfNow() - t0);
#endif
        pthread_mutex_lock(&renderLock);
        renderBusy = 0;
        pthread_cond_broadcast(&renderToGame);
    }
    return NULL;
}

void MiSTer_RenderKick(int32_t smoothRatio)
{
    if (!renderThreadStarted)
    {
        TraceOpen();
        CrashTraceInstall();
        RenderWorldAllocate();
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16 << 20);
        if (pthread_create(&renderThread, &attr, RenderMain, NULL) != 0)
        {
            LOG_F(ERROR, "mister: no thread for the renderer: the game's thread draws");
            renderOn = 0;
            G_DrawRoomsView(smoothRatio);
            return;
        }
        renderThreadStarted = 1;
    }
    MISTER_TRACE(mainPhase, 2);
    RenderWorldCopy();
    MISTER_TRACE(mainPhase, 3);
    pthread_mutex_lock(&renderLock);
    renderBusy = 1;
    renderSmooth = smoothRatio;
    pthread_cond_broadcast(&gameToRender);
    pthread_mutex_unlock(&renderLock);
}

// With the lock held: the sprite animation the renderer asked for, which goes on
static void RenderServe(void)
{
    void (*const fn)(void *) = renderCallFn;
    void *const arg = renderCallArg;

    renderWanted = 0;
    pthread_mutex_unlock(&renderLock);
    if (misterTrace)
        misterTrace->mainRpcDepth++;
    fn(arg);
    MISTER_TRACE(mainRpcDepth, misterTrace->mainRpcDepth - 1);
    pthread_mutex_lock(&renderLock);
    renderServed = 1;
    pthread_cond_broadcast(&gameToRender);
}

void MiSTer_RenderWait(void)
{
#ifdef MISTER_PROFILE
    MISTER_PROF_SCOPE(MPROF_RWAIT);
#endif
    pthread_mutex_lock(&renderLock);
    MISTER_TRACE(mainPhase, 4);
    while (renderBusy)
    {
        if (renderWanted)
            RenderServe();
        else
            pthread_cond_wait(&renderToGame, &renderLock);
    }
    MISTER_TRACE(mainPhase, 5);
    pthread_mutex_unlock(&renderLock);
}

void MiSTer_RenderPoll(void)
{
    if (!renderWanted)
        return;
    pthread_mutex_lock(&renderLock);
    if (renderWanted)
        RenderServe();
    pthread_mutex_unlock(&renderLock);
}

void MiSTer_RenderSync(void)
{
    G_MiSTerViewFinish();
}

void MiSTer_RenderCall(void (*fn)(void *), void *arg)
{
    pthread_mutex_lock(&renderLock);
    renderCallFn = fn;
    renderCallArg = arg;
    renderServed = 0;
    renderWanted = 1;
    MISTER_TRACE(renderPhase, 4);
    pthread_cond_broadcast(&renderToGame);
    while (!renderServed)
        pthread_cond_wait(&gameToRender, &renderLock);
    MISTER_TRACE(renderPhase, 5);
    pthread_mutex_unlock(&renderLock);
}

struct AnimateArgs { int32_t x, y, z, a, smoothratio; };

static void AnimateOnGameThread(void *p)
{
    auto const args = (AnimateArgs *)p;
    if (misterTrace)
        memcpy(misterTrace->rpcArgs, args, sizeof(misterTrace->rpcArgs));
    G_DoSpriteAnimations(args->x, args->y, args->z, args->a, args->smoothratio);
}

void MiSTer_RenderAnimateSprites(int32_t x, int32_t y, int32_t z, int32_t a, int32_t smoothratio)
{
    AnimateArgs args = { x, y, z, a, smoothratio };
    MiSTer_RenderCall(AnimateOnGameThread, &args);
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
