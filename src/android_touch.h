//
// Touch controls + ghost overlay for Crispy Doom on Android / Circle OS.
//

#ifndef ANDROID_TOUCH_H
#define ANDROID_TOUCH_H

#include "SDL.h"

void AT_HandleTouch(SDL_Event *ev);

// Draws the translucent on-screen controls. Call with the game texture already
// copied but before SDL_RenderPresent, so the overlay lands at native panel
// resolution instead of being upscaled with the game framebuffer.
void AT_DrawGhost(SDL_Renderer *r);

// Picks crispy->hires from the panel resolution, once, at startup.
void AT_AutoPickRenderScale(void);

#endif
