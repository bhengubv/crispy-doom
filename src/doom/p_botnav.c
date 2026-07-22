//
// Navigation graph for bot players.
//
// A bot that can only walk toward what it can see is confined to the room it
// spawned in. To go anywhere else it needs to know how the level joins up.
//
// It does not need a new map for that: DOOM already has one. The BSP splits
// every level into convex subsectors, and two subsectors that share a passable
// two-sided line are two places you can walk between. That is a navigation mesh,
// built by the level author and shipped in the WAD. This file reads it out --
// nodes are subsectors, edges are the lines you can cross -- and answers "how do
// I get from here to there" with a breadth-first search over it.
//
// Convexity is what makes it work: inside one subsector there is nothing to
// walk around, so following a route is just walking to each doorway in turn.
// Each edge therefore stores its crossing point, and steering is reduced to
// aiming at the next one. No corner cutting, no wall following.
//
// Doors and lifts are deliberately treated as passable however they happen to
// be sitting when the graph is built. A closed door is not a wall, it is a door;
// the bot walks up to it and presses use, which is what the level intends. The
// alternative -- rebuilding the graph whenever a sector moves -- costs far more
// than being occasionally wrong about a route.
//
// The same graph is what a level editor would need to answer "is the exit
// reachable", which is why it lives in its own file rather than inside the bot.
//

#include <stdlib.h>

#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "p_local.h"
#include "r_state.h"
#include "r_main.h"
#include "tables.h"

#include "p_botnav.h"

#define BN_STEPUP   (24 * FRACUNIT)     // the most a player can climb
#define BN_HEADROOM (56 * FRACUNIT)     // the least a player fits through
#define BN_NUDGE    (12 * FRACUNIT)     // how far past a line counts as across

typedef struct
{
    int     to;
    fixed_t x, y;
} bn_edge_t;

static subsector_t *bn_builtfor;        // the level this graph describes
static int          bn_nnodes;

static int         *bn_first;           // bn_nnodes + 1 entries
static bn_edge_t   *bn_edges;
static int         *bn_visit;           // leveltime a bot was last here

static int         *bn_parent;          // search scratch
static int         *bn_stamp;
static int         *bn_queue;
static int          bn_gen;

// ---------------------------------------------------------------------------
// building
// ---------------------------------------------------------------------------

static void BN_Free(void)
{
    free(bn_first);
    free(bn_edges);
    free(bn_visit);
    free(bn_parent);
    free(bn_stamp);
    free(bn_queue);

    bn_first = NULL;
    bn_edges = NULL;
    bn_visit = NULL;
    bn_parent = NULL;
    bn_stamp = NULL;
    bn_queue = NULL;

    bn_builtfor = NULL;
    bn_nnodes = 0;
}

void BN_Invalidate(void)
{
    BN_Free();
}

// Can a player get from one side of this seg to the other? A line carrying a
// special counts as passable whatever its sectors are doing right now -- that
// is the door-is-not-a-wall rule above.
static boolean BN_SegPassable(const seg_t *seg)
{
    const sector_t *front, *back;
    fixed_t         floor, ceiling;

    if (seg->linedef == NULL || seg->backsector == NULL
     || seg->frontsector == NULL)
    {
        return false;   // one-sided, or a minisegment the nodes builder added
    }

    if (seg->linedef->flags & ML_BLOCKING)
    {
        return false;
    }

    if (seg->linedef->special != 0)
    {
        return true;
    }

    front = seg->frontsector;
    back = seg->backsector;

    if (back->floorheight - front->floorheight > BN_STEPUP)
    {
        return false;   // too high to climb; dropping down is always fine
    }

    floor = back->floorheight > front->floorheight
          ? back->floorheight : front->floorheight;
    ceiling = back->ceilingheight < front->ceilingheight
            ? back->ceilingheight : front->ceilingheight;

    return ceiling - floor >= BN_HEADROOM;
}

// The subsector on the far side of a seg, found by stepping just across it.
// Which way is "across" comes from trying both and taking the one that leaves
// the subsector we started in -- cheaper to test than to reason about, and it
// cannot be got backwards.
static int BN_Across(const seg_t *seg, int from, fixed_t *px, fixed_t *py)
{
    fixed_t mx = seg->v1->x + ((seg->v2->x - seg->v1->x) >> 1);
    fixed_t my = seg->v1->y + ((seg->v2->y - seg->v1->y) >> 1);
    int     side;

    for (side = 0; side < 2; side++)
    {
        angle_t perp = seg->angle + (side ? ANG90 : (angle_t) -ANG90);
        unsigned fine = perp >> ANGLETOFINESHIFT;
        fixed_t x = mx + FixedMul(BN_NUDGE, finecosine[fine]);
        fixed_t y = my + FixedMul(BN_NUDGE, finesine[fine]);
        int     idx = R_PointInSubsector(x, y) - subsectors;

        if (idx != from && idx >= 0 && idx < bn_nnodes)
        {
            *px = x;
            *py = y;
            return idx;
        }
    }

    return BN_NOWHERE;
}

static void BN_Build(void)
{
    int i, j, total;

    BN_Free();

    if (subsectors == NULL || numsubsectors <= 0 || segs == NULL)
    {
        return;
    }

    bn_nnodes = numsubsectors;

    bn_first = calloc(bn_nnodes + 1, sizeof(int));
    bn_visit = calloc(bn_nnodes, sizeof(int));
    bn_parent = calloc(bn_nnodes, sizeof(int));
    bn_stamp = calloc(bn_nnodes, sizeof(int));
    bn_queue = calloc(bn_nnodes, sizeof(int));

    if (bn_first == NULL || bn_visit == NULL || bn_parent == NULL
     || bn_stamp == NULL || bn_queue == NULL)
    {
        BN_Free();
        return;
    }

    // Count first, then fill: the edge list is a flat array indexed by node, so
    // it needs its sizes up front. Two cheap passes beat growing per node.
    for (i = 0; i < bn_nnodes; i++)
    {
        for (j = 0; j < subsectors[i].numlines; j++)
        {
            if (BN_SegPassable(&segs[subsectors[i].firstline + j]))
            {
                bn_first[i + 1]++;
            }
        }
    }

    for (i = 0; i < bn_nnodes; i++)
    {
        bn_first[i + 1] += bn_first[i];
    }

    total = bn_first[bn_nnodes];
    bn_edges = total > 0 ? calloc(total, sizeof(bn_edge_t)) : NULL;

    if (total > 0 && bn_edges == NULL)
    {
        BN_Free();
        return;
    }

    for (i = 0; i < bn_nnodes; i++)
    {
        int slot = bn_first[i];

        for (j = 0; j < subsectors[i].numlines; j++)
        {
            const seg_t *seg = &segs[subsectors[i].firstline + j];
            fixed_t x, y;
            int to;

            if (!BN_SegPassable(seg))
            {
                continue;
            }

            to = BN_Across(seg, i, &x, &y);

            // Keep the slot either way, so the counts still line up; an edge
            // that leads nowhere is simply never followed.
            bn_edges[slot].to = to;
            bn_edges[slot].x = x;
            bn_edges[slot].y = y;
            slot++;
        }
    }

    bn_builtfor = subsectors;
    bn_gen = 0;
}

static boolean BN_Ensure(void)
{
    if (bn_builtfor != subsectors || bn_nnodes != numsubsectors)
    {
        BN_Build();
    }

    return bn_edges != NULL || bn_nnodes > 0;
}

boolean BN_Ready(void)
{
    return BN_Ensure() && bn_first != NULL;
}

int BN_NodeAt(fixed_t x, fixed_t y)
{
    int idx;

    if (!BN_Ready())
    {
        return BN_NOWHERE;
    }

    idx = R_PointInSubsector(x, y) - subsectors;

    return (idx >= 0 && idx < bn_nnodes) ? idx : BN_NOWHERE;
}

void BN_Visit(int node, int now)
{
    if (bn_visit != NULL && node >= 0 && node < bn_nnodes)
    {
        bn_visit[node] = now;
    }
}

boolean BN_PortalPoint(int from, int to, fixed_t *x, fixed_t *y)
{
    int e;

    if (!BN_Ready() || from < 0 || from >= bn_nnodes)
    {
        return false;
    }

    for (e = bn_first[from]; e < bn_first[from + 1]; e++)
    {
        if (bn_edges[e].to == to)
        {
            *x = bn_edges[e].x;
            *y = bn_edges[e].y;
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

// One breadth-first sweep serves both queries. With a goal it stops on the
// goal; without one it stops at the first node nobody has stood in lately,
// which finds the nearest unexplored place and the route to it in a single
// pass. Unweighted BFS is enough because the graph is one room per node --
// fewest rooms crossed is a good enough proxy for shortest, and it costs no
// priority queue.
static int BN_Search(int start, int goal, int *path, int maxlen,
                     int now, int stale)
{
    int head = 0, tail = 0;
    int found = BN_NOWHERE;
    int n, len;

    if (!BN_Ready() || start < 0 || start >= bn_nnodes || maxlen <= 0)
    {
        return 0;
    }

    bn_gen++;
    bn_stamp[start] = bn_gen;
    bn_parent[start] = BN_NOWHERE;
    bn_queue[tail++] = start;

    while (head < tail)
    {
        int cur = bn_queue[head++];
        int e;

        if (goal != BN_NOWHERE)
        {
            if (cur == goal)
            {
                found = cur;
                break;
            }
        }
        else if (cur != start && now - bn_visit[cur] >= stale)
        {
            found = cur;
            break;
        }

        for (e = bn_first[cur]; e < bn_first[cur + 1]; e++)
        {
            int to = bn_edges[e].to;

            if (to < 0 || to >= bn_nnodes || bn_stamp[to] == bn_gen)
            {
                continue;
            }

            bn_stamp[to] = bn_gen;
            bn_parent[to] = cur;
            bn_queue[tail++] = to;
        }
    }

    if (found == BN_NOWHERE)
    {
        return 0;
    }

    // Walk the parents back to the start, then reverse: the caller wants the
    // step it should take next, not the one it would take last.
    len = 0;
    for (n = found; n != start && n != BN_NOWHERE; n = bn_parent[n])
    {
        len++;
    }

    if (len > maxlen)
    {
        return 0;
    }

    n = found;
    for (int i = len - 1; i >= 0; i--)
    {
        path[i] = n;
        n = bn_parent[n];
    }

    return len;
}

int BN_Path(int start, int goal, int *path, int maxlen)
{
    if (goal == BN_NOWHERE)
    {
        return 0;
    }

    return BN_Search(start, goal, path, maxlen, 0, 0);
}

int BN_PathToStale(int start, int *path, int maxlen, int now, int stale)
{
    return BN_Search(start, BN_NOWHERE, path, maxlen, now, stale);
}
