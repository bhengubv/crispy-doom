//
// Per-level record of the hardest bot setting a level has been cleared at.
//

#ifndef __P_BOTREC__
#define __P_BOTREC__

#include <stddef.h>

#include "doomtype.h"

// No rung recorded: the level has never been finished.
#define BR_UNCLEARED (-99)

// Reads the ladder from disk. Safe to call more than once; only the first does
// any work.
void BR_Init(void);

// The best rung for the level being played, or BR_UNCLEARED.
int BR_Best(void);

// Records a completion of the current level at the current setting. Returns
// true if it beat what was there before, and writes the ladder back out.
boolean BR_Record(void);

// Whether the last recorded completion was a new best. For the intermission,
// which draws after the recording has happened.
boolean BR_Improved(void);

// "solo", "2 with you", "1 against" -- the rung in words.
const char *BR_Describe(int balance, char *buf, size_t len);

// One line for the HUD at level start: the rung you are trying to beat.
const char *BR_StatusLine(void);

#endif
