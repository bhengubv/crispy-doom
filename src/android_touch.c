//
// Touch controls for Crispy Doom on Android / Circle OS.
//
//   left 45%      : virtual movement stick (forward / back / strafe)
//   right side    : drag to turn, tap to fire
//   top-left      : ESC  (opens/closes the menu -- always available)
//   bottom-right  : USE  (doors, switches)
//   in menus      : drag = arrow keys, tap = ENTER
//
// SDL finger coords are normalised 0..1, so this is resolution independent.
//

#include "SDL.h"

#include "doomtype.h"
#include "d_event.h"
#include "doomkeys.h"
#include "m_controls.h"
#include "crispy.h"
#include "android_touch.h"

extern boolean menuactive;

#define ZONE_ESC_W   0.14f
#define ZONE_ESC_H   0.18f
#define ZONE_USE_W   0.16f
#define ZONE_USE_H   0.22f
#define STICK_SPLIT  0.45f   // left fraction used as the movement stick
#define MOVE_DEAD    0.045f  // stick deadzone
#define TAP_DIST     0.035f  // max travel that still counts as a tap
#define TAP_TIME     350     // ms
#define TURN_SCALE   1100.0f // finger dx -> mouse turn units

static boolean      inited;
static SDL_FingerID move_id, look_id;
static boolean      move_on, look_on;
static float        move_ox, move_oy;
static float        look_px;            // previous x, for relative turning
static float        look_ox, look_oy;   // origin, for tap detection
static Uint32       look_t0;
static boolean      k_fwd, k_back, k_sl, k_sr;

static void PostKey(evtype_t type, int key)
{
    event_t ev;

    ev.type = type;
    ev.data1 = key;
    ev.data2 = 0;
    ev.data3 = 0;
    ev.data4 = 0;
    D_PostEvent(&ev);
}

static void TapKey(int key)
{
    PostKey(ev_keydown, key);
    PostKey(ev_keyup, key);
}

static void HoldKey(boolean *cur, boolean want, int key)
{
    if (*cur == want)
    {
        return;
    }
    *cur = want;
    PostKey(want ? ev_keydown : ev_keyup, key);
}

static void ReleaseMove(void)
{
    HoldKey(&k_fwd,  false, key_up);
    HoldKey(&k_back, false, key_down);
    HoldKey(&k_sl,   false, key_strafeleft);
    HoldKey(&k_sr,   false, key_straferight);
}

static void PostTurn(int dx)
{
    event_t ev;

    ev.type = ev_mouse;
    ev.data1 = 0;
    ev.data2 = dx;
    ev.data3 = 0;
    ev.data4 = 0;
    D_PostEvent(&ev);
}

static boolean InEsc(float x, float y)
{
    return x < ZONE_ESC_W && y < ZONE_ESC_H;
}

static boolean InUse(float x, float y)
{
    return x > (1.0f - ZONE_USE_W) && y > (1.0f - ZONE_USE_H);
}

void AT_HandleTouch(SDL_Event *ev)
{
    float x, y, dx, dy;
    boolean tap;

    if (!inited)
    {
        inited = true;
        // we translate touch ourselves; don't also get synthetic mouse events
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    }

    x = ev->tfinger.x;
    y = ev->tfinger.y;

    switch (ev->type)
    {
        case SDL_FINGERDOWN:
            if (InEsc(x, y))
            {
                TapKey(KEY_ESCAPE);
                return;
            }

            if (menuactive)
            {
                if (!look_on)
                {
                    look_on = true;
                    look_id = ev->tfinger.fingerId;
                    look_ox = x;
                    look_oy = y;
                    look_t0 = SDL_GetTicks();
                }
                return;
            }

            if (InUse(x, y))
            {
                TapKey(key_use);
                return;
            }

            if (x < STICK_SPLIT)
            {
                if (!move_on)
                {
                    move_on = true;
                    move_id = ev->tfinger.fingerId;
                    move_ox = x;
                    move_oy = y;
                }
            }
            else if (!look_on)
            {
                look_on = true;
                look_id = ev->tfinger.fingerId;
                look_px = x;
                look_ox = x;
                look_oy = y;
                look_t0 = SDL_GetTicks();
            }
            return;

        case SDL_FINGERMOTION:
            if (menuactive)
            {
                return;
            }

            if (move_on && ev->tfinger.fingerId == move_id)
            {
                dx = x - move_ox;
                dy = y - move_oy;
                HoldKey(&k_fwd,  dy < -MOVE_DEAD, key_up);
                HoldKey(&k_back, dy >  MOVE_DEAD, key_down);
                HoldKey(&k_sl,   dx < -MOVE_DEAD, key_strafeleft);
                HoldKey(&k_sr,   dx >  MOVE_DEAD, key_straferight);
            }
            else if (look_on && ev->tfinger.fingerId == look_id)
            {
                PostTurn((int)((x - look_px) * TURN_SCALE));
                look_px = x;
            }
            return;

        case SDL_FINGERUP:
            if (move_on && ev->tfinger.fingerId == move_id)
            {
                move_on = false;
                ReleaseMove();
                return;
            }

            if (look_on && ev->tfinger.fingerId == look_id)
            {
                look_on = false;
                dx = x - look_ox;
                dy = y - look_oy;
                tap = (SDL_GetTicks() - look_t0) < TAP_TIME &&
                      (dx * dx + dy * dy) < (TAP_DIST * TAP_DIST);

                if (menuactive)
                {
                    if (tap)                  TapKey(KEY_ENTER);
                    else if (dy < -TAP_DIST)  TapKey(KEY_UPARROW);
                    else if (dy >  TAP_DIST)  TapKey(KEY_DOWNARROW);
                    else if (dx < -TAP_DIST)  TapKey(KEY_LEFTARROW);
                    else if (dx >  TAP_DIST)  TapKey(KEY_RIGHTARROW);
                    return;
                }

                if (tap)
                {
                    TapKey(key_fire);
                }
            }
            return;

        default:
            return;
    }
}

//
// Pick the internal render scale from the panel, once, at startup.
//
// crispy->hires is used throughout as a SHIFT (ORIGWIDTH << hires), so it
// extends past the stock 0/1 cleanly: 2 gives a 1280x800 base instead of
// 640x400, i.e. 4x the pixels. Don't aim low on hi-res phones -- but keep
// budget devices at 1 so the software renderer stays at framerate.
//
void AT_AutoPickRenderScale(void)
{
    static boolean picked;
    SDL_DisplayMode dm;
    int shortside;

    if (picked)
    {
        return;
    }

    if (SDL_GetCurrentDisplayMode(0, &dm) != 0)
    {
        return;
    }

    shortside = (dm.w < dm.h) ? dm.w : dm.h;

    // MEASURED on a 2340x1080 mid-range device: hires=2 (2080x800 widescreen)
    // is 4x the pixels and drops the software renderer from 35fps to ~20fps,
    // and trips a SIGSEGV from a buffer that doesn't scale with MAXWIDTH.
    // DOOM's renderer is CPU-bound, so more CPU pixels is the wrong lever on a
    // phone -- sharpness comes from the GPU upscale instead. Stay at 1 (640x400
    // base -> ~1040x400 widescreen) and let the shader do the work.
    (void) shortside;
    crispy->hires = 1;
    picked = true;
}
