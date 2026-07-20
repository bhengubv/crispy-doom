//
// GPU edge-directed upscale for Crispy Doom on Android / Circle OS.
//

#ifndef ANDROID_SHADER_H
#define ANDROID_SHADER_H

#include "SDL.h"

#include "doomtype.h"

// True if the Arcade asked for the shader (-shader 1, the default). Whether it
// actually runs on a given frame is further gated in i_video: it only engages
// when the on-screen controls are off, because raw GL and SDL_Renderer cannot
// both draw to the same frame on this device (see the note in android_shader.c).
boolean AS_Enabled(void);

// Upscales src straight to the screen with an edge-directed filter. Returns
// false if it could not -- wrong renderer backend, shader would not build, or
// disabled -- in which case the caller must fall back to crispy's own scaling.
boolean AS_Present(SDL_Renderer *r, SDL_Texture *src, int srcw, int srch);

#endif
