//
// Touch controls for Crispy Doom on Android / Circle OS.
//

#ifndef ANDROID_TOUCH_H
#define ANDROID_TOUCH_H

#include "SDL.h"

void AT_HandleTouch(SDL_Event *ev);

// Picks crispy->hires from the panel resolution, once, at startup.
void AT_AutoPickRenderScale(void);

#endif
