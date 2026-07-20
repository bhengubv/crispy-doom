/* Android-specific stubs for Crispy Doom.
 * The ENDOOM DOS text-mode exit screen makes no sense on mobile (it wants its
 * own SDL text window at exit), so it is disabled here. */

void I_Endoom(unsigned char *data)
{
    (void) data;
}

/* net_gui.c (the multiplayer "waiting for players" textscreen) is excluded on
 * Android; single-player never really reaches this. */
void NET_WaitForLaunch(void)
{
}
