//
// Bot players for Crispy Doom on Circle OS.
//
// A bot is not a monster. It is a *player* -- it occupies one of DOOM's spare
// player slots, spawns at a player start, carries a player's weapons and takes
// a player's damage. All this code does is produce the ticcmd that slot would
// otherwise have received from the network: which way to face, whether to walk,
// whether to pull the trigger.
//
// Doing it at the ticcmd is deliberate, and it is what makes the rest of the
// roadmap possible. Spectator mode is then just a camera pointed at a slot
// nobody is holding, and a netgame is the same slots fed from a socket instead
// of from here. Bots written as a special kind of monster would have bought
// none of that.
//
// It also keeps demos honest: G_Ticker records whatever ends up in the ticcmd,
// so a demo recorded with bots replays as the bots' own commands rather than
// re-running this code and drifting. The bot brain is therefore free to be
// non-deterministic, and has its own RNG so it never disturbs the game's.
//
// What it is not, yet: there is no pathfinding. A bot chases what it can see
// and wanders when it cannot, feeling its way past walls. That is enough to
// fight alongside you and enough to be watched, which is what bots are needed
// for first. Real navigation is the next piece of work, not a missing bit of
// this one.
//

#include <stdlib.h>
#include <string.h>

#include "doomdef.h"
#include "doomstat.h"

#include "d_event.h"
#include "d_player.h"
#include "info.h"
#include "p_local.h"
#include "tables.h"

#include "p_bot.h"
#include "p_botnav.h"

int botcount = 0;

#define BOT_SIGHTRANGE  (2048 * FRACUNIT)
#define BOT_HUNTRANGE   (1280 * FRACUNIT)   // worth walking to, unseen
#define BOT_LONGSHOT    (512 * FRACUNIT)    // past here a shotgun is wasted
#define BOT_FIGHTRANGE  (768 * FRACUNIT)
#define BOT_MELEEHOLD   (192 * FRACUNIT)
#define BOT_LOOKAHEAD   (56 * FRACUNIT)
#define BOT_TURNMAX     1280        // a player's own fast-turn rate
#define BOT_AIMED       (ANG45 / 8) // close enough to shoot
#define BOT_RETARGET    10          // tics between target searches
#define BOT_STUCK       8           // tics of no progress before we give up

#define BOT_MAXPATH     48          // nodes; a route longer than this re-plans
#define BOT_REPATH      70          // tics between route reconsiderations
#define BOT_STALE       (35 * 20)   // a room nobody has entered for this long
                                    // is worth going to look at again
#define BOT_LEASH       (640 * FRACUNIT)    // how far the human may wander off

typedef struct
{
    boolean   active;
    mobj_t   *target;
    int       retarget;
    angle_t   wander;
    int       stucktics;
    fixed_t   lastx, lasty;
    int       strafe;
    int       clock;
    unsigned  rng;

    // Where it is going when there is nothing in front of it to shoot.
    int       path[BOT_MAXPATH];
    int       pathlen;
    int       pathpos;
    int       repath;
    fixed_t   seenx, seeny;     // where a target was when we lost it
    boolean   hasseen;
} bot_t;

static bot_t bots[MAXPLAYERS];

// Bots must never touch P_Random: that sequence is the game's, and demos and
// savegames depend on how far through it we are.
static unsigned BotRandom(bot_t *b)
{
    b->rng ^= b->rng << 13;
    b->rng ^= b->rng >> 17;
    b->rng ^= b->rng << 5;
    return b->rng;
}

// ---------------------------------------------------------------------------
// slots
// ---------------------------------------------------------------------------

boolean P_BotInGame(int playernum)
{
    return playernum >= 0 && playernum < MAXPLAYERS
        && playernum != consoleplayer
        && bots[playernum].active && playeringame[playernum];
}

static void BotClaim(int i)
{
    memset(&bots[i], 0, sizeof(bots[i]));
    bots[i].active = true;
    bots[i].rng = 0x9e3779b9u * (unsigned)(i + 1);

    if (!playeringame[i])
    {
        playeringame[i] = true;
        players[i].playerstate = PST_REBORN;
    }
}

void P_BotInit(void)
{
    int i;

    // The level about to load is a different map to the one the graph was read
    // from. It notices that by itself, but saying so here keeps the stale
    // pointer from ever being reachable.
    BN_Invalidate();

    for (i = 0; i < MAXPLAYERS; i++)
    {
        if (i == consoleplayer)
        {
            continue;
        }

        if (i <= botcount)
        {
            BotClaim(i);
        }
        else
        {
            memset(&bots[i], 0, sizeof(bots[i]));
            playeringame[i] = false;
        }
    }
}

void P_BotSetCount(int n)
{
    int i;

    if (n < 0)
    {
        n = MAXPLAYERS - 1;
    }
    if (n > MAXPLAYERS - 1)
    {
        n = 0;
    }

    botcount = n;

    if (gamestate != GS_LEVEL)
    {
        return;     // P_BotInit will pick it up when the level loads
    }

    for (i = 1; i < MAXPLAYERS; i++)
    {
        if (i == consoleplayer)
        {
            continue;
        }

        if (i <= botcount)
        {
            if (!bots[i].active)
            {
                BotClaim(i);    // G_Ticker's reborn pass spawns it
            }
        }
        else if (bots[i].active)
        {
            // Stop thinking for it and kill the body. Unhooking a live mobj
            // mid-level would leave whatever was chasing it holding a dangling
            // target; letting it die is the path the game already handles, and
            // a bot that stops respawning simply stays down.
            bots[i].active = false;

            if (players[i].mo != NULL && players[i].playerstate == PST_LIVE)
            {
                P_DamageMobj(players[i].mo, NULL, NULL, 10000);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// perception
// ---------------------------------------------------------------------------

static boolean BotWorthShooting(const mobj_t *m, const player_t *self)
{
    if (m->health <= 0 || !(m->flags & MF_SHOOTABLE) || (m->flags & MF_CORPSE))
    {
        return false;
    }

    if (m->player != NULL)
    {
        // Co-op is the default, so hold fire on the other players unless the
        // game is actually a deathmatch.
        return deathmatch && m->player != self;
    }

    // Barrels are shootable but shooting one on sight just makes a bot that
    // walks into rooms and blows itself up.
    return m->type != MT_BARREL;
}

// The nearest thing worth shooting. With needsight the bot must be able to see
// it right now, which is the test for opening fire. Without, it is the test for
// deciding where to walk -- most of the level's monsters are behind a wall at
// any moment, and a bot that only ever considered the visible ones would tour
// the map instead of fighting in it.
static mobj_t *BotNearestEnemy(player_t *p, fixed_t maxdist, boolean needsight)
{
    thinker_t *th;
    mobj_t    *best = NULL;
    fixed_t    bestdist = maxdist;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        mobj_t *m;
        fixed_t dist;

        if (th->function.acp1 != (actionf_p1) P_MobjThinker)
        {
            continue;
        }

        m = (mobj_t *) th;

        if (m == p->mo || !BotWorthShooting(m, p))
        {
            continue;
        }

        dist = P_AproxDistance(m->x - p->mo->x, m->y - p->mo->y);

        if (dist >= bestdist || (needsight && !P_CheckSight(p->mo, m)))
        {
            continue;
        }

        bestdist = dist;
        best = m;
    }

    return best;
}

// Can we walk that way without immediately hitting something? This is a probe,
// not a path: it is what lets a bot slide along a wall instead of pressing into
// it, and nothing more.
static boolean BotClearAhead(mobj_t *mo, angle_t ang)
{
    unsigned fine = ang >> ANGLETOFINESHIFT;

    return P_CheckPosition(mo,
                           mo->x + FixedMul(BOT_LOOKAHEAD, finecosine[fine]),
                           mo->y + FixedMul(BOT_LOOKAHEAD, finesine[fine]));
}

// ---------------------------------------------------------------------------
// navigation
// ---------------------------------------------------------------------------

// Decides where the bot is trying to get to, and asks the graph for the way.
// The order matters more than any of the individual choices: chase down what
// you were just fighting, otherwise stay with the human, otherwise go and look
// somewhere nobody has been. A bot that only ever explored would abandon you
// mid-fight; one that only ever followed would never fetch anything.
static void BotRepath(bot_t *b, player_t *p, mobj_t *mo, int here)
{
    int goal = BN_NOWHERE;

    b->repath = BOT_REPATH;
    b->pathpos = 0;
    b->pathlen = 0;

    if (b->hasseen)
    {
        goal = BN_NodeAt(b->seenx, b->seeny);

        if (goal == here || goal == BN_NOWHERE)
        {
            b->hasseen = false;     // arrived, and it was not there
            goal = BN_NOWHERE;
        }
    }

    // Go and find a fight. Almost every monster on the level is behind a wall
    // at any given moment, so without this the bot spends its life exploring
    // past them -- which is exactly what it did before this existed.
    if (goal == BN_NOWHERE)
    {
        const mobj_t *hunt = BotNearestEnemy(p, BOT_HUNTRANGE, false);

        if (hunt != NULL)
        {
            goal = BN_NodeAt(hunt->x, hunt->y);
        }
    }

    if (goal == BN_NOWHERE && !deathmatch && playeringame[consoleplayer])
    {
        const mobj_t *lead = players[consoleplayer].mo;

        if (lead != NULL && players[consoleplayer].playerstate == PST_LIVE
         && P_AproxDistance(lead->x - mo->x, lead->y - mo->y) > BOT_LEASH)
        {
            goal = BN_NodeAt(lead->x, lead->y);
        }
    }

    if (goal != BN_NOWHERE && goal != here)
    {
        b->pathlen = BN_Path(here, goal, b->path, BOT_MAXPATH);
    }

    if (b->pathlen == 0)
    {
        b->pathlen = BN_PathToStale(here, b->path, BOT_MAXPATH,
                                    leveltime, BOT_STALE);
    }
}

// Returns the direction to walk. Following a route is only ever "aim at the
// next doorway", because a subsector is convex: there is nothing inside one to
// walk around.
static angle_t BotNavigate(bot_t *b, player_t *p, mobj_t *mo)
{
    int     here = BN_NodeAt(mo->x, mo->y);
    fixed_t px, py;

    if (here == BN_NOWHERE)
    {
        return b->wander;   // no graph; fall through to feeling for walls
    }

    BN_Visit(here, leveltime);

    // Drop the nodes we have already walked into.
    while (b->pathpos < b->pathlen && b->path[b->pathpos] == here)
    {
        b->pathpos++;
    }

    if (b->pathpos >= b->pathlen || --b->repath <= 0)
    {
        BotRepath(b, p, mo, here);
    }

    if (b->pathpos < b->pathlen
     && BN_PortalPoint(here, b->path[b->pathpos], &px, &py))
    {
        return R_PointToAngle2(mo->x, mo->y, px, py);
    }

    // Off the route -- knocked back, fell, or the door shut behind us. Plan
    // again from wherever we actually are.
    b->repath = 0;
    return b->wander;
}

// ---------------------------------------------------------------------------
// weapons
// ---------------------------------------------------------------------------

static void BotChooseWeapon(player_t *p, ticcmd_t *cmd, fixed_t dist)
{
    // Deliberately no rocket launcher or BFG: a bot with no notion of splash
    // radius kills itself with them, and does it often enough to be the first
    // thing anyone notices. The shotgun entry also gets the super shotgun,
    // which the engine swaps in by itself when both are owned.
    static const weapontype_t close[] = {
        wp_plasma, wp_shotgun, wp_chaingun, wp_pistol, wp_chainsaw, wp_fist
    };
    // Past a certain range a shotgun's spread throws most of its pellets away,
    // so reach for the chaingun instead.
    static const weapontype_t far[] = {
        wp_plasma, wp_chaingun, wp_shotgun, wp_pistol, wp_chainsaw, wp_fist
    };
    const weapontype_t *order = (dist > BOT_LONGSHOT) ? far : close;
    size_t i;

    for (i = 0; i < sizeof(close) / sizeof(close[0]); i++)
    {
        weapontype_t w = order[i];
        ammotype_t   a = weaponinfo[w].ammo;

        if (!p->weaponowned[w])
        {
            continue;
        }
        if (a != am_noammo && p->ammo[a] <= 0)
        {
            continue;
        }

        if (p->readyweapon != w && p->pendingweapon == wp_nochange)
        {
            cmd->buttons |= BT_CHANGE | (w << BT_WEAPONSHIFT);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// the tic
// ---------------------------------------------------------------------------

void P_BotTiccmd(int playernum, ticcmd_t *cmd)
{
    bot_t    *b = &bots[playernum];
    player_t *p = &players[playernum];
    mobj_t   *mo = p->mo;
    angle_t   want;
    fixed_t   dist = 0;
    int       delta, turn;
    boolean   engaging = false;

    memset(cmd, 0, sizeof(*cmd));

    b->clock++;

    if (mo == NULL || p->playerstate != PST_LIVE)
    {
        // Dead. Ask to respawn the way a player does, but not instantly --
        // watching a bot pop back up the frame it dies looks like a glitch.
        if (p->playerstate == PST_DEAD && b->active && (b->clock & 31) == 0)
        {
            cmd->buttons |= BT_USE;
        }
        b->target = NULL;
        return;
    }

    // --- who are we fighting -----------------------------------------------
    if (b->target != NULL
     && (!BotWorthShooting(b->target, p) || !P_CheckSight(mo, b->target)))
    {
        // Remember where it went. Losing sight of something is the main reason
        // a bot needs to navigate at all -- without this it forgets the fight
        // the instant the enemy steps behind a pillar.
        if (b->target->health > 0)
        {
            b->seenx = b->target->x;
            b->seeny = b->target->y;
            b->hasseen = true;
        }

        b->target = NULL;
    }

    // Whatever just hurt us outranks whatever we were looking at. The engine
    // records it for free, and without this a bot being shot from across a room
    // carries on exploring as though nothing were happening -- which was the
    // single most inert-looking thing they did.
    if (p->attacker != NULL && p->attacker != mo
     && BotWorthShooting(p->attacker, p))
    {
        if (P_CheckSight(mo, p->attacker))
        {
            b->target = p->attacker;
        }
        else if (b->target == NULL)
        {
            b->seenx = p->attacker->x;
            b->seeny = p->attacker->y;
            b->hasseen = true;
        }
    }

    if (--b->retarget <= 0)
    {
        b->retarget = BOT_RETARGET;

        if (b->target == NULL)
        {
            b->target = BotNearestEnemy(p, BOT_SIGHTRANGE, true);
        }
    }

    if (b->target != NULL)
    {
        dist = P_AproxDistance(b->target->x - mo->x, b->target->y - mo->y);
        want = R_PointToAngle2(mo->x, mo->y, b->target->x, b->target->y);
        engaging = true;
    }
    else
    {
        // Nothing in sight, so go somewhere. The graph decides where; feeling
        // along the wall is only what happens when it has no answer.
        if (b->wander == 0 && b->clock < 2)
        {
            b->wander = mo->angle;
        }

        want = BotNavigate(b, p, mo);

        if (want == b->wander && !BotClearAhead(mo, b->wander))
        {
            b->wander += (BotRandom(b) & 1) ? ANG45 : (angle_t) -ANG45;
            want = b->wander;
        }
    }

    BotChooseWeapon(p, cmd, engaging ? dist : BOT_LONGSHOT);

    // --- face it ------------------------------------------------------------
    delta = (int)(want - mo->angle);
    turn = delta / 65536;

    if (turn > BOT_TURNMAX)
    {
        turn = BOT_TURNMAX;
    }
    else if (turn < -BOT_TURNMAX)
    {
        turn = -BOT_TURNMAX;
    }

    cmd->angleturn = (short) turn;

    // --- move ---------------------------------------------------------------
    if (engaging && dist < BOT_MELEEHOLD && weaponinfo[p->readyweapon].ammo != am_noammo)
    {
        cmd->forwardmove = 0;   // close enough; don't crowd it
    }
    else if (BotClearAhead(mo, mo->angle))
    {
        cmd->forwardmove = (engaging && dist < BOT_FIGHTRANGE) ? 25 : 50;
    }
    else
    {
        // Blocked. Sidestep rather than grind into the wall.
        cmd->forwardmove = 0;
        b->strafe = (BotRandom(b) & 1) ? 1 : -1;
    }

    if (engaging && dist < BOT_FIGHTRANGE)
    {
        // Circle-strafe while fighting. A bot that walks straight at things is
        // both easy to hit and dull to watch.
        if ((b->clock % 35) == 0)
        {
            b->strafe = (BotRandom(b) & 1) ? 1 : -1;
        }
        cmd->sidemove = (signed char)(b->strafe * 40);
    }
    else if (b->strafe != 0)
    {
        cmd->sidemove = (signed char)(b->strafe * 40);
        if ((b->clock & 7) == 0)
        {
            b->strafe = 0;
        }
    }

    // --- shoot --------------------------------------------------------------
    if (engaging && dist < BOT_SIGHTRANGE && abs(delta) < (int) BOT_AIMED)
    {
        // Ask the engine what this shot would actually hit, using the same
        // auto-aim the trigger pull will use. If the answer is a teammate,
        // don't pull it. Co-op friendly fire is on in DOOM, and a bot happy to
        // empty a shotgun through your back is worse than no bot at all.
        P_AimLineAttack(mo, mo->angle, BOT_SIGHTRANGE);

        if (deathmatch || linetarget == NULL || linetarget->player == NULL)
        {
            cmd->buttons |= BT_ATTACK;
        }
    }

    // --- unstick ------------------------------------------------------------
    if (abs(mo->x - b->lastx) + abs(mo->y - b->lasty) < 2 * FRACUNIT)
    {
        if (++b->stucktics > BOT_STUCK)
        {
            b->stucktics = 0;
            b->wander = mo->angle + ((BotRandom(b) & 1) ? ANG90 : (angle_t) -ANG90);
            b->target = NULL;
            b->strafe = (BotRandom(b) & 1) ? 1 : -1;

            // Doors and lifts read as walls to a bot with no map knowledge, so
            // try the wall it is stuck against before assuming it is solid.
            cmd->buttons |= BT_USE;
        }
    }
    else
    {
        b->stucktics = 0;
    }

    b->lastx = mo->x;
    b->lasty = mo->y;
}
