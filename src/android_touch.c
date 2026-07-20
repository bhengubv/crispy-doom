//
// Touch controls + PSP-style ghost overlay for Crispy Doom on Android / Circle OS.
//
//   left 45%      : virtual movement stick (forward / back / strafe), drawn as a nub
//   right side    : drag to turn, tap to fire
//   face cluster  : O = fire, X = use, ^ = next weapon, [] = prev weapon
//   top-left      : MENU (ESC) -- always available, even in menus
//   in menus      : drag = arrow keys, tap = ENTER
//
// SDL finger coords are normalised 0..1, so hit-testing is resolution
// independent. The overlay is drawn with the SDL renderer AFTER the game
// texture has been copied, so it lands at native panel resolution rather than
// being upscaled with the 320x200-lineage framebuffer -- and it overlays the
// game rather than insetting it, so edge-to-edge play is preserved.
//
// The Arcade passes user settings in on the launch intent, which DoomActivity
// turns into argv:  -ghost <0|1>  -ghostalpha <0..100>
//

#include <math.h>
#include <stdlib.h>

#include "SDL.h"

#include "doomtype.h"
#include "d_event.h"
#include "doomkeys.h"
#include "m_argv.h"
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

// Ghost overlay geometry. x is a fraction of window width, y a fraction of
// window height, and every radius is a fraction of window HEIGHT so the
// buttons stay circular on any aspect ratio.
#define FACE_CX      0.850f
#define FACE_CY      0.660f
#define FACE_OFF     0.170f
#define BTN_R        0.085f
#define NUB_CX       0.115f
#define NUB_CY       0.680f
#define NUB_R        0.150f
#define NUB_DOT_R    0.055f
#define TOUCH_SLOP   1.25f   // buttons hit-test slightly larger than they draw

enum { B_FIRE, B_USE, B_NEXT, B_PREV, B_MENU, NBTN };

static struct {
    float        cx, cy, r;
    boolean      down;
    SDL_FingerID finger;
} btn[NBTN];

static boolean      inited;
static SDL_FingerID move_id, look_id;
static boolean      move_on, look_on;
static float        move_ox, move_oy;
static float        look_px;            // previous x, for relative turning
static float        look_ox, look_oy;   // origin, for tap detection
static Uint32       look_t0;
static boolean      k_fwd, k_back, k_sl, k_sr;

// Ghost overlay state.
static int          ghost_on = 1;
static int          ghost_alpha = 89;   // 0..255 (35% by default)
static boolean      ghost_args_read;
static float        cur_aspect = 2.0f;
static float        nub_dx, nub_dy;     // nub travel, in height units
static SDL_Texture *tex_disc, *tex_ring, *tex_o, *tex_x, *tex_tri, *tex_sq,
                   *tex_menu;
static boolean      tex_tried;

// ---------------------------------------------------------------------------
// event helpers
// ---------------------------------------------------------------------------

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
    nub_dx = 0.0f;
    nub_dy = 0.0f;
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

// ---------------------------------------------------------------------------
// layout + settings
// ---------------------------------------------------------------------------

static void ReadGhostArgs(void)
{
    int p;

    if (ghost_args_read)
    {
        return;
    }
    ghost_args_read = true;

    //!
    // @arg <0|1>
    //
    // Show the translucent on-screen controls. Passed by the Circle OS Arcade.
    //
    p = M_CheckParmWithArgs("-ghost", 1);
    if (p > 0)
    {
        ghost_on = atoi(myargv[p + 1]) ? 1 : 0;
    }

    //!
    // @arg <0-100>
    //
    // Opacity of the on-screen controls, as a percentage.
    //
    p = M_CheckParmWithArgs("-ghostalpha", 1);
    if (p > 0)
    {
        int pct = atoi(myargv[p + 1]);

        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        ghost_alpha = (pct * 255) / 100;
    }
}

static void Layout(void)
{
    float off = FACE_OFF / cur_aspect;  // horizontal offset in width units

    btn[B_FIRE].cx = FACE_CX + off;  btn[B_FIRE].cy = FACE_CY;
    btn[B_FIRE].r  = BTN_R * 1.15f;

    btn[B_PREV].cx = FACE_CX - off;  btn[B_PREV].cy = FACE_CY;
    btn[B_PREV].r  = BTN_R * 0.80f;

    btn[B_NEXT].cx = FACE_CX;        btn[B_NEXT].cy = FACE_CY - FACE_OFF;
    btn[B_NEXT].r  = BTN_R * 0.80f;

    btn[B_USE].cx  = FACE_CX;        btn[B_USE].cy  = FACE_CY + FACE_OFF;
    btn[B_USE].r   = BTN_R * 0.95f;

    btn[B_MENU].cx = 0.055f;         btn[B_MENU].cy = 0.110f;
    btn[B_MENU].r  = BTN_R * 0.62f;
}

static int ButtonKey(int i)
{
    switch (i)
    {
        case B_FIRE: return key_fire;
        case B_USE:  return key_use;
        case B_NEXT: return key_nextweapon;
        case B_PREV: return key_prevweapon;
        default:     return KEY_ESCAPE;
    }
}

static int HitButton(float fx, float fy)
{
    int   i;
    float dx, dy, rr;

    if (!ghost_on)
    {
        return -1;
    }

    for (i = 0; i < NBTN; i++)
    {
        // While a menu is up only MENU is live; the rest would fight the
        // drag/tap menu navigation.
        if (menuactive && i != B_MENU)
        {
            continue;
        }

        dx = (fx - btn[i].cx) * cur_aspect;
        dy = (fy - btn[i].cy);
        rr = btn[i].r * TOUCH_SLOP;

        if (dx * dx + dy * dy <= rr * rr)
        {
            return i;
        }
    }

    return -1;
}

// ---------------------------------------------------------------------------
// shape rasteriser
//
// Each shape is evaluated in unit coords (-1..1) and supersampled 3x3, so the
// edges come out smooth. One white texture per shape; colour and opacity are
// applied at draw time with colour/alpha mod.
// ---------------------------------------------------------------------------

#define TEXDIM 192

typedef float (*shape_fn)(float x, float y);

static float sh_disc(float x, float y)
{
    return (x * x + y * y <= 1.0f) ? 1.0f : 0.0f;
}

static float sh_ring(float x, float y)
{
    float d = sqrtf(x * x + y * y);
    return (d <= 1.0f && d >= 0.86f) ? 1.0f : 0.0f;
}

static float sh_o(float x, float y)
{
    float d = sqrtf(x * x + y * y);
    return (d <= 0.72f && d >= 0.50f) ? 1.0f : 0.0f;
}

static float sh_x(float x, float y)
{
    const float t = 0.155f;

    if (fabsf(x) > 0.62f || fabsf(y) > 0.62f)
    {
        return 0.0f;
    }
    return (fabsf(x - y) < t || fabsf(x + y) < t) ? 1.0f : 0.0f;
}

static float sh_sq(float x, float y)
{
    float m = (fabsf(x) > fabsf(y)) ? fabsf(x) : fabsf(y);
    return (m <= 0.62f && m >= 0.45f) ? 1.0f : 0.0f;
}

static int tri_in(float x, float y, float s)
{
    float ax = 0.00f, ay = -0.66f;
    float bx = 0.60f, by =  0.42f;
    float cx = -0.60f, cy =  0.42f;
    const float gx = 0.0f, gy = 0.06f;   // centroid, scaled about
    float e1, e2, e3;

    ax = gx + (ax - gx) * s;  ay = gy + (ay - gy) * s;
    bx = gx + (bx - gx) * s;  by = gy + (by - gy) * s;
    cx = gx + (cx - gx) * s;  cy = gy + (cy - gy) * s;

    e1 = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
    e2 = (cx - bx) * (y - by) - (cy - by) * (x - bx);
    e3 = (ax - cx) * (y - cy) - (ay - cy) * (x - cx);

    return (e1 >= 0.0f && e2 >= 0.0f && e3 >= 0.0f) ||
           (e1 <= 0.0f && e2 <= 0.0f && e3 <= 0.0f);
}

static float sh_tri(float x, float y)
{
    return (tri_in(x, y, 1.0f) && !tri_in(x, y, 0.68f)) ? 1.0f : 0.0f;
}

static float sh_menu(float x, float y)
{
    if (fabsf(x) > 0.58f)
    {
        return 0.0f;
    }
    if (fabsf(y + 0.34f) < 0.10f) return 1.0f;
    if (fabsf(y)         < 0.10f) return 1.0f;
    if (fabsf(y - 0.34f) < 0.10f) return 1.0f;
    return 0.0f;
}

static SDL_Texture *MakeShape(SDL_Renderer *r, shape_fn f)
{
    SDL_Texture *t;
    Uint32      *px;
    int          i, j, sx, sy;

    px = malloc(TEXDIM * TEXDIM * sizeof(Uint32));

    if (px == NULL)
    {
        return NULL;
    }

    for (j = 0; j < TEXDIM; j++)
    {
        for (i = 0; i < TEXDIM; i++)
        {
            float acc = 0.0f;
            Uint8 a;

            for (sy = 0; sy < 3; sy++)
            {
                for (sx = 0; sx < 3; sx++)
                {
                    float u = ((i + (sx + 0.5f) / 3.0f) / TEXDIM) * 2.0f - 1.0f;
                    float v = ((j + (sy + 0.5f) / 3.0f) / TEXDIM) * 2.0f - 1.0f;

                    acc += f(u, v);
                }
            }

            a = (Uint8)((acc / 9.0f) * 255.0f + 0.5f);
            px[j * TEXDIM + i] = ((Uint32) a << 24) | 0x00ffffffu;
        }
    }

    t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                          SDL_TEXTUREACCESS_STATIC, TEXDIM, TEXDIM);

    if (t != NULL)
    {
        SDL_UpdateTexture(t, NULL, px, TEXDIM * sizeof(Uint32));
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }

    free(px);

    return t;
}

static void EnsureTextures(SDL_Renderer *r)
{
    if (tex_tried)
    {
        return;
    }
    tex_tried = true;

    tex_disc = MakeShape(r, sh_disc);
    tex_ring = MakeShape(r, sh_ring);
    tex_o    = MakeShape(r, sh_o);
    tex_x    = MakeShape(r, sh_x);
    tex_tri  = MakeShape(r, sh_tri);
    tex_sq   = MakeShape(r, sh_sq);
    tex_menu = MakeShape(r, sh_menu);
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

static void DrawTex(SDL_Renderer *r, SDL_Texture *t, float cxpx, float cypx,
                    float rpx, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 a)
{
    SDL_Rect dst;

    if (t == NULL)
    {
        return;
    }

    dst.x = (int)(cxpx - rpx);
    dst.y = (int)(cypx - rpx);
    dst.w = (int)(rpx * 2.0f);
    dst.h = dst.w;

    SDL_SetTextureColorMod(t, cr, cg, cb);
    SDL_SetTextureAlphaMod(t, a);
    SDL_RenderCopy(r, t, NULL, &dst);
}

void AT_DrawGhost(SDL_Renderer *r)
{
    int      w, h, i, lw, lh;
    SDL_bool iscale;
    Uint8    a_base, a_line;

    ReadGhostArgs();

    if (!ghost_on || r == NULL)
    {
        return;
    }

    if (SDL_GetRendererOutputSize(r, &w, &h) != 0 || w <= 0 || h <= 0)
    {
        return;
    }

    // crispy puts a logical size on the renderer so the game frame scales and
    // letterboxes. Drop it while we draw, then restore. Without this the
    // overlay is laid out in panel pixels but drawn through the logical-space
    // transform, which both mislocates it and resamples it with the
    // 320x200-lineage framebuffer. Drawing in real panel pixels is the whole
    // point -- it is what keeps the controls crisp on a hi-res phone.
    SDL_RenderGetLogicalSize(r, &lw, &lh);
    iscale = SDL_RenderGetIntegerScale(r);

    if (lw > 0 && lh > 0)
    {
        SDL_RenderSetLogicalSize(r, 0, 0);
    }

    cur_aspect = (float) w / (float) h;
    Layout();
    EnsureTextures(r);

    // The user's opacity applies to the bright parts directly; the filled body
    // sits well under it so the controls read as a ghost, not a HUD.
    a_base = (Uint8)(ghost_alpha * 0.45f);
    a_line = (Uint8) ghost_alpha;

    // Movement nub -- outer ring plus a dot that follows the thumb.
    if (!menuactive)
    {
        float ncx = NUB_CX * w;
        float ncy = NUB_CY * h;

        DrawTex(r, tex_disc, ncx, ncy, NUB_R * h, 44, 62, 80, a_base);
        DrawTex(r, tex_ring, ncx, ncy, NUB_R * h, 33, 150, 243, a_line);
        DrawTex(r, tex_disc,
                ncx + nub_dx * h, ncy + nub_dy * h,
                NUB_DOT_R * h, 33, 150, 243, a_line);
    }

    for (i = 0; i < NBTN; i++)
    {
        SDL_Texture *sym;
        float        cxpx, cypx, rpx;
        Uint8        la = a_line;

        if (menuactive && i != B_MENU)
        {
            continue;
        }

        switch (i)
        {
            case B_FIRE: sym = tex_o;    break;
            case B_USE:  sym = tex_x;    break;
            case B_NEXT: sym = tex_tri;  break;
            case B_PREV: sym = tex_sq;   break;
            default:     sym = tex_menu; break;
        }

        cxpx = btn[i].cx * w;
        cypx = btn[i].cy * h;
        rpx  = btn[i].r  * h;

        // Pressed buttons read brighter, so a thumb gets feedback even at a
        // low opacity setting.
        if (btn[i].down)
        {
            la = (Uint8)(ghost_alpha + (255 - ghost_alpha) * 0.85f);
        }

        DrawTex(r, tex_disc, cxpx, cypx, rpx, 44, 62, 80, a_base);
        DrawTex(r, tex_ring, cxpx, cypx, rpx, 33, 150, 243, la);
        DrawTex(r, sym,      cxpx, cypx, rpx, 255, 255, 255, la);
    }

    if (lw > 0 && lh > 0)
    {
        SDL_RenderSetLogicalSize(r, lw, lh);
        SDL_RenderSetIntegerScale(r, iscale);
    }
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

void AT_HandleTouch(SDL_Event *ev)
{
    float   x, y, dx, dy;
    boolean tap;
    int     b, i;

    if (!inited)
    {
        inited = true;
        // we translate touch ourselves; don't also get synthetic mouse events
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
        ReadGhostArgs();
        Layout();
    }

    x = ev->tfinger.x;
    y = ev->tfinger.y;

    switch (ev->type)
    {
        case SDL_FINGERDOWN:
            b = HitButton(x, y);

            if (b >= 0)
            {
                if (b == B_MENU)
                {
                    TapKey(KEY_ESCAPE);
                }
                else
                {
                    btn[b].down = true;
                    btn[b].finger = ev->tfinger.fingerId;
                    PostKey(ev_keydown, ButtonKey(b));
                }
                return;
            }

            if (!ghost_on && InEsc(x, y))
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

            if (!ghost_on && InUse(x, y))
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

                // Feed the nub visual. Travel is in height units and clamped
                // to the ring so the dot never escapes its track.
                {
                    float tx = dx * cur_aspect;
                    float ty = dy;
                    float len = sqrtf(tx * tx + ty * ty);
                    float lim = NUB_R - NUB_DOT_R;

                    if (len > lim && len > 0.0f)
                    {
                        tx *= lim / len;
                        ty *= lim / len;
                    }
                    nub_dx = tx;
                    nub_dy = ty;
                }
            }
            else if (look_on && ev->tfinger.fingerId == look_id)
            {
                PostTurn((int)((x - look_px) * TURN_SCALE));
                look_px = x;
            }
            return;

        case SDL_FINGERUP:
            for (i = 0; i < NBTN; i++)
            {
                if (btn[i].down && ev->tfinger.fingerId == btn[i].finger)
                {
                    btn[i].down = false;
                    PostKey(ev_keyup, ButtonKey(i));
                    return;
                }
            }

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
// 640x400, i.e. 4x the pixels.
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
