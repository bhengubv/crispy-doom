/* Android-specific pieces of Crispy Doom.
 * The ENDOOM DOS text-mode exit screen makes no sense on mobile (it wants its
 * own SDL text window at exit), so it is disabled here. */

#include <string.h>

#include "doomdef.h"
#include "doomtype.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_menu.h"
#include "m_misc.h"
#include "net_client.h"
#include "net_defs.h"
#include "net_server.h"

void I_Endoom(unsigned char *data)
{
    (void) data;
}

/* net_gui.c is the DOS-style textscreen lobby, which is excluded on Android --
 * it wants a terminal we do not have and a player list nobody wants to read on
 * a phone. This is the same wait, drawn with the game's own renderer.
 *
 * The host launches as soon as a second player arrives. Two players is the
 * game, so there is nothing to decide and nothing to press. Both sides give up
 * after a minute rather than hanging on a network that is not going to answer.
 */

#define NET_WAIT_SECONDS 60

void NET_WaitForLaunch(void)
{
    int deadline = I_GetTime() + NET_WAIT_SECONDS * TICRATE;

    while (net_waiting_for_launch)
    {
        char line[80];
        int  dots;

        NET_CL_Run();
        NET_SV_Run();

        if (!net_client_connected)
        {
            return;
        }

        if (I_GetTime() > deadline)
        {
            NET_CL_Disconnect();
            return;
        }

        if (net_client_received_wait_data
         && net_client_wait_data.is_controller
         && net_client_wait_data.num_players >= 2)
        {
            NET_CL_LaunchGame();
        }

        if ((I_GetTime() % TICRATE) == 0)

        dots = (I_GetTime() / (TICRATE / 2)) % 4;

        if (net_client_received_wait_data)
        {
            M_snprintf(line, sizeof(line), "%d of %d players%.*s",
                       net_client_wait_data.num_players,
                       net_client_wait_data.max_players, dots, "...");
        }
        else
        {
            M_snprintf(line, sizeof(line), "Looking for a game%.*s", dots, "...");
        }

        memset(I_VideoBuffer, 0,
               SCREENWIDTH * SCREENHEIGHT * sizeof(*I_VideoBuffer));

        M_WriteText(ORIGWIDTH / 2 - M_StringWidth(line) / 2,
                    ORIGHEIGHT / 2 - 4, line);

        I_FinishUpdate();
        I_Sleep(20);
    }
}
