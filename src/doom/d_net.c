//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	DOOM Network game communication and protocol,
//	all OS independend parts.
//

#include <stdlib.h>

#include "d_main.h"
#include "m_argv.h"
#include "m_menu.h"
#include "m_misc.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "g_game.h"
#include "doomdef.h"
#include "doomstat.h"
#include "p_bot.h"
#include "w_checksum.h"
#include "w_wad.h"

#include "deh_main.h"

#include "d_loop.h"
#include "g_game.h"
#include "net_client.h"
#include "net_gui.h"
#include "net_io.h"
#include "net_loop.h"
#include "net_query.h"
#include "net_sdl.h"
#include "net_server.h"

ticcmd_t *netcmds;

// Called when a player leaves the game

static void PlayerQuitGame(player_t *player)
{
    static char exitmsg[80];
    unsigned int player_num;

    player_num = player - players;

    // Do this the same way as Vanilla Doom does, to allow dehacked
    // replacements of this message

    M_StringCopy(exitmsg, DEH_String("Player 1 left the game"),
                 sizeof(exitmsg));

    exitmsg[7] += player_num;

    playeringame[player_num] = false;
    players[consoleplayer].message = exitmsg;
    // [crispy] don't interpolate players who left the game
    player->mo->interp = false;

    // TODO: check if it is sensible to do this:

    if (demorecording) 
    {
        G_CheckDemoStatus ();
    }
}

static void RunTic(ticcmd_t *cmds, boolean *ingame)
{
    unsigned int i;

    // Check for player quits.

    for (i = 0; i < MAXPLAYERS; ++i)
    {
        // [circle] A bot holds a player slot the network layer knows nothing
        // about, so the single-player path reports it as not in the game on
        // every tic. Without this exemption the bot is quit out one tic after
        // it spawns -- which is exactly what it did.
        // A quit for a player who never got a body is nothing to clean up, and
        // PlayerQuitGame assumes there is one.
        if (!demoplayback && playeringame[i] && !ingame[i]
         && players[i].mo != NULL && !P_BotInGame(i))
        {
            PlayerQuitGame(&players[i]);
        }
    }

    netcmds = cmds;

    // check that there are players in the game.  if not, we cannot
    // run a tic.

    if (advancedemo)
        D_DoAdvanceDemo ();

    G_Ticker ();
}

static loop_interface_t doom_loop_interface = {
    D_ProcessEvents,
    G_BuildTiccmd,
    RunTic,
    M_Ticker
};


// Load game settings from the specified structure and
// set global variables.

static void LoadGameSettings(net_gamesettings_t *settings)
{
    unsigned int i;

    deathmatch = settings->deathmatch;
    startepisode = settings->episode;
    startmap = settings->map;
    startskill = settings->skill;
    startloadgame = settings->loadgame;
    lowres_turn = settings->lowres_turn;
    nomonsters = settings->nomonsters;
    fastparm = settings->fast_monsters;
    respawnparm = settings->respawn_monsters;
    timelimit = settings->timelimit;
    consoleplayer = settings->consoleplayer;

    if (lowres_turn)
    {
        printf("NOTE: Turning resolution is reduced; this is probably "
               "because there is a client recording a Vanilla demo.\n");
    }

    for (i = 0; i < MAXPLAYERS; ++i)
    {
        playeringame[i] = i < settings->num_players;
    }
}

// Save the game settings from global variables to the specified
// game settings structure.

static void SaveGameSettings(net_gamesettings_t *settings)
{
    // Fill in game settings structure with appropriate parameters
    // for the new game

    settings->deathmatch = deathmatch;
    settings->episode = startepisode;
    settings->map = startmap;
    settings->skill = startskill;
    settings->loadgame = startloadgame;
    settings->gameversion = gameversion;
    settings->nomonsters = nomonsters;
    settings->fast_monsters = fastparm;
    settings->respawn_monsters = respawnparm;
    settings->timelimit = timelimit;

    settings->lowres_turn = (M_ParmExists("-record")
                         && !M_ParmExists("-longtics"))
                          || M_ParmExists("-shorttics");
}

static void InitConnectData(net_connect_data_t *connect_data)
{
    boolean shorttics;

    connect_data->max_players = MAXPLAYERS;
    connect_data->drone = false;

    //!
    // @category net
    //
    // Run as the left screen in three screen mode.
    //

    if (M_CheckParm("-left") > 0)
    {
        viewangleoffset = ANG90;
        connect_data->drone = true;
    }

    //! 
    // @category net
    //
    // Run as the right screen in three screen mode.
    //

    if (M_CheckParm("-right") > 0)
    {
        viewangleoffset = ANG270;
        connect_data->drone = true;
    }

    //
    // Connect data
    //

    // Game type fields:

    connect_data->gamemode = gamemode;
    connect_data->gamemission = gamemission;

    //!
    // @category demo
    //
    // Play with low turning resolution to emulate demo recording.
    //

    shorttics = M_ParmExists("-shorttics");

    // Are we recording a demo? Possibly set lowres turn mode

    connect_data->lowres_turn = (M_ParmExists("-record")
                             && !M_ParmExists("-longtics"))
                              || shorttics;

    // Read checksums of our WAD directory and dehacked information

    W_Checksum(connect_data->wad_sha1sum);
    DEH_Checksum(connect_data->deh_sha1sum);

    // Are we playing with the Freedoom IWAD?

    connect_data->is_freedoom = W_CheckNumForName("FREEDOOM") >= 0;
}

void D_ConnectNetGame(void)
{
    net_connect_data_t connect_data;

    InitConnectData(&connect_data);
    netgame = D_InitNetGame(&connect_data);

    //!
    // @category net
    //
    // Start the game playing as though in a netgame with a single
    // player.  This can also be used to play back single player netgame
    // demos.
    //

    if (M_CheckParm("-solo-net") > 0)
    {
        netgame = true;
    }
}

//
// D_CheckNetGame
// Works out player numbers among the net participants
//

// [circle] --- starting a netgame after the game is already running ---------
//
// Vanilla decides netgame-or-not at startup, in D_InitNetGame, from the command
// line -- long before a menu exists. A phone has no command line, so there is
// no reachable path to a netgame at all. These run the same sequence on demand:
// bring a server up (or go and find one), connect, wait for the launch, then
// negotiate settings exactly as D_CheckNetGame does at boot, and start the map
// everyone agreed on.
//
// Deliberately no lobby. Two players is the game, so the host launches the
// moment somebody arrives; a phone has nowhere good to put a player list and
// nobody wants to read one while holding the thing sideways.

static boolean D_NetStart(net_addr_t *addr)
{
    net_connect_data_t connect_data;
    net_gamesettings_t settings;

    if (addr == NULL)
    {
        return false;
    }

    InitConnectData(&connect_data);

    if (!NET_CL_Connect(addr, &connect_data))
    {
        NET_ReleaseAddress(addr);
        return false;
    }

    NET_ReleaseAddress(addr);

    // Blocks, drawing its own screen, until the game is launched or it gives up
    NET_WaitForLaunch();


    if (!net_client_connected)
    {
        return false;
    }

    netgame = true;
    autostart = true;

    SaveGameSettings(&settings);
    D_StartNetGame(&settings, NULL);
    LoadGameSettings(&settings);

    // [circle] The loop has been running since the title screen, so its tic
    // counters are far ahead of a server that starts counting at zero.
    // Without this the client dies with "lowtic < gametic" the moment it
    // tries to run a tic. Startup never has to think about it: nothing has
    // run yet.
    D_ResetLoop();


    G_DeferedInitNew(startskill, startepisode, startmap);

    return true;
}

boolean D_NetHost(void)
{
    net_addr_t *addr;

    NET_SV_Init();
    NET_SV_AddModule(&net_loop_server_module);
    NET_SV_AddModule(&net_sdl_module);

    // The host plays too, so it joins its own server through the loopback
    // module rather than going out over the wire to itself.
    net_loop_client_module.InitClient();
    addr = net_loop_client_module.ResolveAddress(NULL);
    NET_ReferenceAddress(addr);

    return D_NetStart(addr);
}

boolean D_NetJoin(void)
{
    // Broadcast for a server on the LAN. Nobody is typing an IP address into a
    // phone, and on a mesh there may not be one worth typing.
    return D_NetStart(NET_FindLANServer());
}

void D_CheckNetGame (void)
{
    net_gamesettings_t settings;

    if (netgame)
    {
        autostart = true;
    }

    D_RegisterLoopCallbacks(&doom_loop_interface);

    SaveGameSettings(&settings);
    D_StartNetGame(&settings, NULL);
    LoadGameSettings(&settings);

    DEH_printf("startskill %i  deathmatch: %i  startmap: %i  startepisode: %i\n",
               startskill, deathmatch, startmap, startepisode);

    DEH_printf("player %i of %i (%i nodes)\n",
               consoleplayer+1, settings.num_players, settings.num_players);

    // Show players here; the server might have specified a time limit

    if (timelimit > 0 && deathmatch)
    {
        // Gross hack to work like Vanilla:

        if (timelimit == 20 && M_CheckParm("-avg"))
        {
            DEH_printf("Austin Virtual Gaming: Levels will end "
                           "after 20 minutes\n");
        }
        else
        {
            DEH_printf("Levels will end after %d minute", timelimit);
            if (timelimit > 1)
                printf("s");
            printf(".\n");
        }
    }
}

