//
// Per-level record of the hardest bot setting a level has been cleared at.
//
// The bot dial is only a difficulty setting until something remembers where you
// left it. With a record it becomes a ladder: clear a map with three marines
// helping, then two, then one, then alone, then with one hunting you. Same map
// each time, and no content had to be authored for any of it.
//
// So the record is deliberately the *furthest right* you have ever finished at,
// not the last setting you used. Sliding back left to get past a hard bit does
// not cost you anything -- the rung you reached is yours. That asymmetry is the
// whole point: the dial is training wheels you take off yourself, and nothing
// here should punish you for putting them back on for an afternoon.
//
// Kept in its own small text file next to the config rather than inside it,
// because it is a save file, not a preference: it grows with the WADs you play
// and it is the one thing here worth not losing.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomdef.h"
#include "doomstat.h"

#include "m_config.h"
#include "m_misc.h"

#include "p_bot.h"
#include "p_botrec.h"

#define BR_MAXENTRIES 512
#define BR_FILENAME   "botladder.txt"

typedef struct
{
    short mission, episode, map;
    short best;
} br_entry_t;

static br_entry_t br_entries[BR_MAXENTRIES];
static int        br_count;
static boolean    br_loaded;
static boolean    br_improved;

static char *BR_Path(void)
{
    return M_StringJoin(configdir != NULL ? configdir : "", BR_FILENAME, NULL);
}

static br_entry_t *BR_Find(boolean create)
{
    int i;

    for (i = 0; i < br_count; i++)
    {
        if (br_entries[i].mission == (short) gamemission
         && br_entries[i].episode == (short) gameepisode
         && br_entries[i].map == (short) gamemap)
        {
            return &br_entries[i];
        }
    }

    if (!create || br_count >= BR_MAXENTRIES)
    {
        return NULL;
    }

    br_entries[br_count].mission = (short) gamemission;
    br_entries[br_count].episode = (short) gameepisode;
    br_entries[br_count].map = (short) gamemap;
    br_entries[br_count].best = BR_UNCLEARED;

    return &br_entries[br_count++];
}

static void BR_Save(void)
{
    char *path = BR_Path();
    FILE *f;
    int   i;

    if (path == NULL)
    {
        return;
    }

    f = M_fopen(path, "w");

    if (f != NULL)
    {
        fprintf(f, "# Circle OS bot ladder. mission episode map best\n");
        fprintf(f, "# best: negative marines with you, positive against, 0 solo\n");

        for (i = 0; i < br_count; i++)
        {
            if (br_entries[i].best != BR_UNCLEARED)
            {
                fprintf(f, "%d %d %d %d\n", br_entries[i].mission,
                        br_entries[i].episode, br_entries[i].map,
                        br_entries[i].best);
            }
        }

        fclose(f);
    }

    free(path);
}

void BR_Init(void)
{
    char *path;
    FILE *f;
    char  line[128];

    if (br_loaded)
    {
        return;
    }

    br_loaded = true;
    path = BR_Path();

    if (path == NULL)
    {
        return;
    }

    f = M_fopen(path, "r");

    if (f != NULL)
    {
        while (fgets(line, sizeof(line), f) != NULL && br_count < BR_MAXENTRIES)
        {
            int mission, episode, map, best;

            if (line[0] == '#'
             || sscanf(line, "%d %d %d %d", &mission, &episode, &map, &best) != 4)
            {
                continue;
            }

            br_entries[br_count].mission = (short) mission;
            br_entries[br_count].episode = (short) episode;
            br_entries[br_count].map = (short) map;
            br_entries[br_count].best = (short) best;
            br_count++;
        }

        fclose(f);
    }

    free(path);
}

int BR_Best(void)
{
    const br_entry_t *e;

    BR_Init();
    e = BR_Find(false);

    return e != NULL ? e->best : BR_UNCLEARED;
}

boolean BR_Record(void)
{
    br_entry_t *e;

    BR_Init();

    // Only the human's ladder is being kept, and only for a real playthrough.
    // A demo playing back is not somebody clearing a level.
    if (demoplayback || netgame)
    {
        br_improved = false;
        return false;
    }

    e = BR_Find(true);

    if (e == NULL)
    {
        br_improved = false;
        return false;
    }

    br_improved = (e->best == BR_UNCLEARED || botbalance > e->best);

    if (br_improved)
    {
        e->best = (short) botbalance;
        BR_Save();
    }

    return br_improved;
}

boolean BR_Improved(void)
{
    return br_improved;
}

const char *BR_StatusLine(void)
{
    static char line[64];
    char        what[48];
    int         best = BR_Best();

    if (best == BR_UNCLEARED)
    {
        M_StringCopy(line, "Not cleared yet", sizeof(line));
    }
    else
    {
        M_snprintf(line, sizeof(line), "Best: %s",
                   BR_Describe(best, what, sizeof(what)));
    }

    return line;
}

const char *BR_Describe(int balance, char *buf, size_t len)
{
    if (balance == BR_UNCLEARED)
    {
        M_StringCopy(buf, "not cleared yet", len);
    }
    else if (balance < 0)
    {
        M_snprintf(buf, len, "%d marine%s with you",
                   -balance, balance == -1 ? "" : "s");
    }
    else if (balance > 0)
    {
        M_snprintf(buf, len, "%d marine%s against you",
                   balance, balance == 1 ? "" : "s");
    }
    else
    {
        M_StringCopy(buf, "solo", len);
    }

    return buf;
}
