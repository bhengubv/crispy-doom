//
// Bot players for Crispy Doom on Circle OS.
//

#ifndef __P_BOT__
#define __P_BOT__

#include "doomdef.h"
#include "doomtype.h"
#include "d_ticcmd.h"

// One dial for the whole thing: negative is that many marines fighting with
// you, positive is that many hunting you, zero is alone. Its magnitude is how
// many player slots get filled, so the four-player limit enforces itself and
// there is no combination to get wrong.
//
// Both halves are the same game -- same map, same monsters. Only whose side the
// other marines are on changes, so a run at -2 and a run at +2 are comparable.
extern int botbalance;

#define BOT_MAXBALANCE (MAXPLAYERS - 1)

// Claims or releases bot slots for the level about to load. Call before
// P_SetupLevel, which is what actually spawns the players it finds claimed.
void P_BotInit(void);

// Menu hook. Takes effect immediately if a level is running.
void P_BotSetBalance(int n);

// True while the bots are hunting the human rather than helping.
boolean P_BotHostile(void);

// Watch the marines play instead of playing. The human stays in their slot but
// stops mattering: untargetable, unkillable, and not the view.
extern boolean botspectate;

// Picks whose eyes to watch through. Call once per tic.
void P_BotSpectate(void);

boolean P_BotInGame(int playernum);

// Fills in the slot's command for this tic, in place of the network's.
void P_BotTiccmd(int playernum, ticcmd_t *cmd);

#endif
