//
// Native-resolution text for Crispy Doom on Android / Circle OS.
//

#ifndef ANDROID_TEXT_H
#define ANDROID_TEXT_H

#include "SDL.h"

#include "doomtype.h"

// Records one character instead of blitting it. x/y/cellw/cellh are the
// vanilla patch coordinates and metrics the engine already computed, in
// 320x200 space; ch is the character before toupper(), so mixed-case strings
// keep their case; trans is the current dp_translation, or NULL.
//
// Returns true if the glyph was taken. Returns false when it cannot be drawn
// (no such glyph, or the queue is full), and the caller must fall back to
// V_DrawPatchDirect so text is never silently lost.
boolean AX_Glyph(int x, int y, int cellw, int cellh, char ch,
                 const byte *trans);

// Records a slider. x/y/w/h are the vanilla patch coordinates of the whole
// control in 320x200 space; frac is 0..1. Returns false if it cannot be taken,
// and the caller must fall back to the M_THERM* patches.
boolean AX_Thermo(int x, int y, int w, int h, float frac);

// Replays and clears the queue, drawing at true panel resolution. Call after
// the game frame has been copied to the renderer, before present.
void AX_Draw(SDL_Renderer *r);

#endif
