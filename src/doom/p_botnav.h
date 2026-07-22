//
// Navigation graph for bot players.
//

#ifndef __P_BOTNAV__
#define __P_BOTNAV__

#include "doomtype.h"
#include "m_fixed.h"

#define BN_NOWHERE (-1)

// Throws away the current graph. The next query rebuilds it. Called on level
// load; the graph also notices a level change on its own, so a missed call
// costs nothing.
void BN_Invalidate(void);

// True once a graph exists for the level that is loaded now.
boolean BN_Ready(void);

// Which node a point sits in, or BN_NOWHERE.
int BN_NodeAt(fixed_t x, fixed_t y);

// Shortest route from start to goal. Writes node indices after start into
// path and returns how many, or 0 if there is no way through.
int BN_Path(int start, int goal, int *path, int maxlen);

// Route to the nearest node no bot has stood in for `stale` tics. Same
// return as BN_Path; the goal is whatever it finds.
int BN_PathToStale(int start, int *path, int maxlen, int now, int stale);

// Records that a bot is standing here, so the others look elsewhere.
void BN_Visit(int node, int now);

// The point to walk to in order to cross from one node into the next.
boolean BN_PortalPoint(int from, int to, fixed_t *x, fixed_t *y);

#endif
