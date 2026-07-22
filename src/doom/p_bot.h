//
// Bot players for Crispy Doom on Circle OS.
//

#ifndef __P_BOT__
#define __P_BOT__

#include "doomtype.h"
#include "d_ticcmd.h"

// How many of the spare player slots to fill. 0 disables bots entirely.
extern int botcount;

// Claims or releases bot slots for the level about to load. Call before
// P_SetupLevel, which is what actually spawns the players it finds claimed.
void P_BotInit(void);

// Menu hook. Takes effect immediately if a level is running.
void P_BotSetCount(int n);

boolean P_BotInGame(int playernum);

// Fills in the slot's command for this tic, in place of the network's.
void P_BotTiccmd(int playernum, ticcmd_t *cmd);

#endif
