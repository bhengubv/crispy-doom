//
// Touch controls + PSP-style ghost overlay for Crispy Doom on Android / Circle OS.
//
// The overlay is meant to read as one device, in the Arcade's visual language:
// STEEL bodies with a vertical gradient lit from above, an ACCENT hairline rim,
// a top-edge highlight, and lettering baked from the Arcade's own ui_font.ttf.
//
//   top corners   : L / R shoulders -- strafe left / right
//   top centre    : SELECT (automap) and START (menu)
//   left upper    : D-pad -- discrete movement
//   left lower    : analog nub in a dished track -- absolute, PSP style
//   right         : face diamond -- O fire, X use, triangle/square weapon cycle
//   right open    : drag to turn, tap to fire
//   in menus      : drag = arrow keys, tap = ENTER; only START stays live
//
// SDL finger coords are normalised 0..1, so hit-testing is resolution
// independent. Drawing happens with the SDL renderer after the game texture is
// copied and with crispy's logical size temporarily dropped, so the controls
// land in true panel pixels -- crisp on a hi-res phone, and overlaying the
// frame rather than insetting it, which keeps edge-to-edge play.
//
// The Arcade owns the settings and passes them on the launch intent, which
// DoomActivity turns into argv:  -ghost <0|1>  -ghostalpha <0..100>
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
#include "android_labels.h"

extern boolean menuactive;

#define TAP_DIST     0.035f  // max travel that still counts as a tap
#define TAP_TIME     350     // ms
#define TURN_SCALE   1100.0f // finger dx -> mouse turn units
#define TOUCH_SLOP   1.20f   // hit targets run slightly larger than they draw

// --- layout. x is a fraction of window width; y and every size are fractions
// --- of window height, so shapes stay proportional on any aspect.
#define SHOULDER_Y   0.080f
#define SHOULDER_HW  0.085f
#define SHOULDER_HH  0.040f
#define SHOULDER_LX  0.060f
#define SHOULDER_RX  0.940f

#define PILL_Y       0.078f
#define PILL_HH      0.042f
#define PILL_TEXT_H  0.036f
#define PILL_PAD     0.026f
#define SELECT_X     0.440f
#define START_X      0.560f

// The D-pad and the face cluster share CLUSTER_Y. On a PSP those two sit at
// exactly the same height, mirrored across the centre line, and the eye reads
// any drift between them as the whole overlay being tilted.
#define CLUSTER_Y    0.460f

#define DPAD_CX      0.072f
#define DPAD_CY      CLUSTER_Y
#define DPAD_ARM     0.115f
#define DPAD_WAIST   0.038f   // half-thickness of the cross bars

// DOOM's status bar owns the bottom of the frame -- 32 of its 200 rows, so 16%
// -- and the player reads health and ammo off it constantly. Controls are held
// clear of it with a margin rather than overlapping it for a few pixels of
// extra reach.
#define HUD_TOP      0.840f
#define HUD_MARGIN   0.028f
#define SAFE_BOTTOM  (HUD_TOP - HUD_MARGIN)

// The nub is deliberately smaller than the D-pad, as on the hardware -- it is a
// thumb slider, not the hero. Sized larger it dominates the left side and drags
// the whole composition down with it. It is the lowest control, so it is the
// one pinned to the safe area.
#define NUB_CX       0.105f
#define NUB_TRACK    0.098f
#define NUB_CAP      0.042f
#define NUB_CY       (SAFE_BOTTOM - NUB_TRACK)
#define NUB_DEAD     0.015f

#define FACE_CX      0.868f
#define FACE_CY      CLUSTER_Y
#define FACE_OFF     0.175f
#define FACE_R_BIG   0.100f
#define FACE_R_MID   0.090f
#define FACE_R_SML   0.078f

// --- palette. STEEL and ACCENT are the Arcade's. The four face symbols use the
// --- authentic PlayStation colours; the user signed that off specifically,
// --- because the colour coding is most of why a face cluster is readable at a
// --- glance and size alone could not carry it. Nothing else deviates.
#define STEEL_R   44
#define STEEL_G   62
#define STEEL_B   80
#define ACC_R     33
#define ACC_G    150
#define ACC_B    243

enum { B_FIRE, B_USE, B_NEXT, B_PREV, B_L, B_R, B_START, B_SELECT, NBTN };

typedef struct {
    float        cx, cy;     // centre
    float        r;          // radius (discs) or half-height (pills)
    float        hw;         // half-width, pills only; 0 for discs
    boolean      pill;
    boolean      down;
    SDL_FingerID finger;
} gbtn_t;

static gbtn_t btn[NBTN];

static boolean      inited;
static SDL_FingerID look_id, dpad_id, nub_id;
static boolean      look_on, dpad_on, nub_on;
static float        look_px;
static float        look_ox, look_oy;
static Uint32       look_t0;
static boolean      k_fwd, k_back, k_sl, k_sr;
static float        nub_dx, nub_dy;   // cap offset from centre, height units

static int          ghost_on = 1;
static int          ghost_alpha = 89;   // 0..255 (35% by default)
static boolean      ghost_args_read;
static float        cur_aspect = 2.0f;

static SDL_Texture *tex_layer;          // offscreen compose target
static int          layer_w, layer_h;
static SDL_Texture *tex_disc_g, *tex_disc_f, *tex_bar_g, *tex_bar_f;
static SDL_Texture *tex_dpad_g, *tex_dpad_f, *tex_track, *tex_hl, *tex_dish;
static SDL_Texture *tex_o, *tex_x, *tex_tri, *tex_sq, *tex_tri_s;
static SDL_Texture *tex_lbl[LBL_NUM];
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

// ---------------------------------------------------------------------------
// settings + layout
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

static float LabelHalfWidth(int idx)
{
    float ar = (float) at_labels[idx].w / (float) at_labels[idx].h;

    return (PILL_TEXT_H * ar) * 0.5f + PILL_PAD;
}

static void Layout(void)
{
    float off = FACE_OFF / cur_aspect;   // horizontal offset in width units
    int   i;

    for (i = 0; i < NBTN; i++)
    {
        btn[i].pill = false;
        btn[i].hw = 0.0f;
    }

    btn[B_FIRE].cx = FACE_CX + off;  btn[B_FIRE].cy = FACE_CY;
    btn[B_FIRE].r  = FACE_R_BIG;

    btn[B_PREV].cx = FACE_CX - off;  btn[B_PREV].cy = FACE_CY;
    btn[B_PREV].r  = FACE_R_SML;

    btn[B_NEXT].cx = FACE_CX;        btn[B_NEXT].cy = FACE_CY - FACE_OFF;
    btn[B_NEXT].r  = FACE_R_SML;

    btn[B_USE].cx  = FACE_CX;        btn[B_USE].cy  = FACE_CY + FACE_OFF;
    btn[B_USE].r   = FACE_R_MID;

    btn[B_L].cx = SHOULDER_LX;  btn[B_L].cy = SHOULDER_Y;
    btn[B_L].r  = SHOULDER_HH;  btn[B_L].hw = SHOULDER_HW;  btn[B_L].pill = true;

    btn[B_R].cx = SHOULDER_RX;  btn[B_R].cy = SHOULDER_Y;
    btn[B_R].r  = SHOULDER_HH;  btn[B_R].hw = SHOULDER_HW;  btn[B_R].pill = true;

    btn[B_SELECT].cx = SELECT_X;  btn[B_SELECT].cy = PILL_Y;
    btn[B_SELECT].r  = PILL_HH;   btn[B_SELECT].pill = true;
    btn[B_SELECT].hw = LabelHalfWidth(LBL_SELECT);

    btn[B_START].cx = START_X;    btn[B_START].cy = PILL_Y;
    btn[B_START].r  = PILL_HH;    btn[B_START].pill = true;
    btn[B_START].hw = LabelHalfWidth(LBL_START);
}

static int ButtonKey(int i)
{
    switch (i)
    {
        case B_FIRE:   return key_fire;
        case B_USE:    return key_use;
        case B_NEXT:   return key_nextweapon;
        case B_PREV:   return key_prevweapon;
        case B_L:      return key_strafeleft;
        case B_R:      return key_straferight;
        case B_SELECT: return key_map_toggle;
        default:       return KEY_ESCAPE;
    }
}

static boolean HitOne(int i, float fx, float fy)
{
    float dx = (fx - btn[i].cx) * cur_aspect;
    float dy = (fy - btn[i].cy);

    if (btn[i].pill)
    {
        float hw = btn[i].hw * TOUCH_SLOP;
        float hh = btn[i].r * TOUCH_SLOP;

        return fabsf(dx) <= hw && fabsf(dy) <= hh;
    }
    else
    {
        float rr = btn[i].r * TOUCH_SLOP;

        return (dx * dx + dy * dy) <= (rr * rr);
    }
}

static int HitButton(float fx, float fy)
{
    int i;

    if (!ghost_on)
    {
        return -1;
    }

    for (i = 0; i < NBTN; i++)
    {
        // While a menu is up only START is live; the rest would fight the
        // drag/tap menu navigation.
        if (menuactive && i != B_START)
        {
            continue;
        }
        if (HitOne(i, fx, fy))
        {
            return i;
        }
    }

    return -1;
}

static boolean HitDpad(float fx, float fy)
{
    float dx = (fx - DPAD_CX) * cur_aspect;
    float dy = (fy - DPAD_CY);
    float a  = DPAD_ARM * TOUCH_SLOP;

    return fabsf(dx) <= a && fabsf(dy) <= a;
}

static boolean HitNub(float fx, float fy)
{
    float dx = (fx - NUB_CX) * cur_aspect;
    float dy = (fy - NUB_CY);
    float rr = NUB_TRACK * TOUCH_SLOP;

    return (dx * dx + dy * dy) <= (rr * rr);
}

static void ApplyDpad(float fx, float fy)
{
    float dx = (fx - DPAD_CX) * cur_aspect;
    float dy = (fy - DPAD_CY);

    // Dominant axis wins, so a thumb near a diagonal does not stutter between
    // two directions the way a naive per-axis threshold does.
    if (fabsf(dx) > fabsf(dy))
    {
        HoldKey(&k_fwd,  false, key_up);
        HoldKey(&k_back, false, key_down);
        HoldKey(&k_sl,   dx < 0.0f, key_strafeleft);
        HoldKey(&k_sr,   dx > 0.0f, key_straferight);
    }
    else
    {
        HoldKey(&k_sl,   false, key_strafeleft);
        HoldKey(&k_sr,   false, key_straferight);
        HoldKey(&k_fwd,  dy < 0.0f, key_up);
        HoldKey(&k_back, dy > 0.0f, key_down);
    }
}

static void ApplyNub(float fx, float fy)
{
    float dx  = (fx - NUB_CX) * cur_aspect;
    float dy  = (fy - NUB_CY);
    float lim = NUB_TRACK - NUB_CAP;
    float len = sqrtf(dx * dx + dy * dy);

    // Absolute, like a real nub: the cap sits where the thumb is, clamped to
    // its track, rather than tracking a relative drag from first touch.
    if (len > lim && len > 0.0f)
    {
        dx *= lim / len;
        dy *= lim / len;
    }
    nub_dx = dx;
    nub_dy = dy;

    HoldKey(&k_fwd,  dy < -NUB_DEAD, key_up);
    HoldKey(&k_back, dy >  NUB_DEAD, key_down);
    HoldKey(&k_sl,   dx < -NUB_DEAD, key_strafeleft);
    HoldKey(&k_sr,   dx >  NUB_DEAD, key_straferight);
}

// ---------------------------------------------------------------------------
// shape rasteriser
//
// Shapes are evaluated in unit coords (-1..1) and supersampled 3x3 so edges
// come out smooth. Bodies bake a vertical luminance ramp into RGB -- lit from
// above, like a real button -- so a colour mod tints the whole gradient at
// once. Flat variants stay white for rims, symbols and highlights.
// ---------------------------------------------------------------------------

#define TEXDIM 192

typedef float (*shape_fn)(float x, float y);

static float sh_disc(float x, float y)
{
    return (x * x + y * y <= 1.0f) ? 1.0f : 0.0f;
}

static float sh_full(float x, float y)
{
    (void) x; (void) y;
    return 1.0f;
}

static float sh_track(float x, float y)
{
    float d = sqrtf(x * x + y * y);
    return (d <= 1.0f && d >= 0.88f) ? 1.0f : 0.0f;
}

// A sheen hugging the inside of the top edge. It has to be thin and it has to
// fade out along its length -- a thick arc with hard ends reads as a grey band
// smeared across the button rather than as light catching an edge.
static float sh_hl(float x, float y)
{
    float d = sqrtf(x * x + y * y);
    float taper;

    if (d > 0.955f || d < 0.865f || y > -0.05f)
    {
        return 0.0f;
    }

    taper = (-y - 0.05f) / 0.80f;

    if (taper > 1.0f)
    {
        taper = 1.0f;
    }

    return taper * taper;
}

static float sh_cross(float x, float y)
{
    float wst = DPAD_WAIST / DPAD_ARM;   // waist as a fraction of the arm

    return (fabsf(x) <= wst || fabsf(y) <= wst) ? 1.0f : 0.0f;
}

static float sh_o(float x, float y)
{
    float d = sqrtf(x * x + y * y);
    return (d <= 0.70f && d >= 0.545f) ? 1.0f : 0.0f;
}

static float sh_x(float x, float y)
{
    const float t = 0.155f;

    if (fabsf(x) > 0.50f || fabsf(y) > 0.50f)
    {
        return 0.0f;
    }
    return (fabsf(x - y) < t || fabsf(x + y) < t) ? 1.0f : 0.0f;
}

// Slightly smaller than the other three: a square's corners reach further than
// a circle's edge, so matching their nominal radius makes it look oversized.
static float sh_sq(float x, float y)
{
    float m = (fabsf(x) > fabsf(y)) ? fabsf(x) : fabsf(y);
    return (m <= 0.54f && m >= 0.385f) ? 1.0f : 0.0f;
}

static int tri_in(float x, float y, float s)
{
    float ax = 0.00f,  ay = -0.64f;
    float bx = 0.58f,  by =  0.40f;
    float cx = -0.58f, cy =  0.40f;
    const float gx = 0.0f, gy = 0.05f;
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
    return (tri_in(x, y, 1.0f) && !tri_in(x, y, 0.62f)) ? 1.0f : 0.0f;
}

// Solid, for the D-pad's direction chevrons.
static float sh_tri_s(float x, float y)
{
    return tri_in(x, y, 1.0f) ? 1.0f : 0.0f;
}

// grad: 0 = flat white, 1 = convex (lit from above), 2 = concave (the inverse,
// which is what makes the nub's dish read as recessed under a convex cap).
static SDL_Texture *MakeShape(SDL_Renderer *r, shape_fn f, int grad)
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
        float v = ((j + 0.5f) / TEXDIM) * 2.0f - 1.0f;
        float lum;
        Uint8 c;

        if (grad == 1)
        {
            lum = 1.0f - 0.55f * ((v + 1.0f) * 0.5f);
        }
        else if (grad == 2)
        {
            lum = 0.45f + 0.55f * ((v + 1.0f) * 0.5f);
        }
        else
        {
            lum = 1.0f;
        }
        c = (Uint8)(lum * 255.0f + 0.5f);

        for (i = 0; i < TEXDIM; i++)
        {
            float acc = 0.0f;
            Uint8 a;

            for (sy = 0; sy < 3; sy++)
            {
                for (sx = 0; sx < 3; sx++)
                {
                    float u = ((i + (sx + 0.5f) / 3.0f) / TEXDIM) * 2.0f - 1.0f;
                    float w = ((j + (sy + 0.5f) / 3.0f) / TEXDIM) * 2.0f - 1.0f;

                    acc += f(u, w);
                }
            }

            a = (Uint8)((acc / 9.0f) * 255.0f + 0.5f);
            px[j * TEXDIM + i] = ((Uint32) a << 24) |
                                 ((Uint32) c << 16) | ((Uint32) c << 8) | c;
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

// Expand one RLE-packed label from android_labels.h into a white texture whose
// alpha is the glyph coverage.
static SDL_Texture *MakeLabel(SDL_Renderer *r, const atlabel_t *L)
{
    SDL_Texture *t;
    Uint32      *px;
    int          i, n, o;

    px = malloc(L->w * L->h * sizeof(Uint32));

    if (px == NULL)
    {
        return NULL;
    }

    o = 0;
    for (i = 0; i + 1 < L->rlen && o < L->w * L->h; i += 2)
    {
        int   cnt = L->rle[i];
        Uint8 val = L->rle[i + 1];
        Uint32 v = ((Uint32) val << 24) | 0x00ffffffu;

        for (n = 0; n < cnt && o < L->w * L->h; n++)
        {
            px[o++] = v;
        }
    }
    while (o < L->w * L->h)
    {
        px[o++] = 0;
    }

    t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                          SDL_TEXTUREACCESS_STATIC, L->w, L->h);

    if (t != NULL)
    {
        SDL_UpdateTexture(t, NULL, px, L->w * sizeof(Uint32));
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }

    free(px);

    return t;
}

static void EnsureTextures(SDL_Renderer *r)
{
    int i;

    if (tex_tried)
    {
        return;
    }
    tex_tried = true;

    tex_disc_g = MakeShape(r, sh_disc,  1);
    tex_disc_f = MakeShape(r, sh_disc,  0);
    tex_dish   = MakeShape(r, sh_disc,  2);
    tex_bar_g  = MakeShape(r, sh_full,  1);
    tex_bar_f  = MakeShape(r, sh_full,  0);
    tex_dpad_g = MakeShape(r, sh_cross, 1);
    tex_dpad_f = MakeShape(r, sh_cross, 0);
    tex_track  = MakeShape(r, sh_track, 0);
    tex_hl     = MakeShape(r, sh_hl,    0);
    tex_o      = MakeShape(r, sh_o,     0);
    tex_x      = MakeShape(r, sh_x,     0);
    tex_tri    = MakeShape(r, sh_tri,   0);
    tex_tri_s  = MakeShape(r, sh_tri_s, 0);
    tex_sq     = MakeShape(r, sh_sq,    0);

    for (i = 0; i < LBL_NUM; i++)
    {
        tex_lbl[i] = MakeLabel(r, &at_labels[i]);
    }
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

static void DrawSq(SDL_Renderer *r, SDL_Texture *t, float cx, float cy,
                   float rad, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 a)
{
    SDL_Rect dst;

    if (t == NULL)
    {
        return;
    }

    dst.x = (int)(cx - rad);
    dst.y = (int)(cy - rad);
    dst.w = (int)(rad * 2.0f);
    dst.h = dst.w;

    SDL_SetTextureColorMod(t, cr, cg, cb);
    SDL_SetTextureAlphaMod(t, a);
    SDL_RenderCopy(r, t, NULL, &dst);
}

static void DrawSqRot(SDL_Renderer *r, SDL_Texture *t, float cx, float cy,
                      float rad, double angle,
                      Uint8 cr, Uint8 cg, Uint8 cb, Uint8 a)
{
    SDL_Rect dst;

    if (t == NULL)
    {
        return;
    }

    dst.x = (int)(cx - rad);
    dst.y = (int)(cy - rad);
    dst.w = (int)(rad * 2.0f);
    dst.h = dst.w;

    SDL_SetTextureColorMod(t, cr, cg, cb);
    SDL_SetTextureAlphaMod(t, a);
    SDL_RenderCopyEx(r, t, NULL, &dst, angle, NULL, SDL_FLIP_NONE);
}

static void DrawRect(SDL_Renderer *r, SDL_Texture *t, float cx, float cy,
                     float hw, float hh, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 a)
{
    SDL_Rect dst;

    if (t == NULL || hw <= 0.0f)
    {
        return;
    }

    dst.x = (int)(cx - hw);
    dst.y = (int)(cy - hh);
    dst.w = (int)(hw * 2.0f);
    dst.h = (int)(hh * 2.0f);

    SDL_SetTextureColorMod(t, cr, cg, cb);
    SDL_SetTextureAlphaMod(t, a);
    SDL_RenderCopy(r, t, NULL, &dst);
}

// A pill is two caps plus a bar. All three share the same vertical gradient, so
// stretching the bar horizontally stays seamless.
static void DrawPill(SDL_Renderer *r, boolean grad, float cx, float cy,
                     float hw, float hh, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 a)
{
    SDL_Texture *cap = grad ? tex_disc_g : tex_disc_f;
    SDL_Texture *bar = grad ? tex_bar_g  : tex_bar_f;
    float        mid = hw - hh;

    DrawSq(r, cap, cx - mid, cy, hh, cr, cg, cb, a);
    DrawSq(r, cap, cx + mid, cy, hh, cr, cg, cb, a);

    if (mid > 0.0f)
    {
        DrawRect(r, bar, cx, cy, mid, hh, cr, cg, cb, a);
    }
}

static void DrawLabel(SDL_Renderer *r, int idx, float cx, float cy,
                      float texth, Uint8 a)
{
    const atlabel_t *L = &at_labels[idx];
    SDL_Rect         dst;

    if (tex_lbl[idx] == NULL)
    {
        return;
    }

    dst.h = (int) texth;
    dst.w = (int)(texth * ((float) L->w / (float) L->h));
    dst.x = (int)(cx - dst.w * 0.5f);
    dst.y = (int)(cy - dst.h * 0.5f);

    SDL_SetTextureColorMod(tex_lbl[idx], 255, 255, 255);
    SDL_SetTextureAlphaMod(tex_lbl[idx], a);
    SDL_RenderCopy(r, tex_lbl[idx], NULL, &dst);
}

// Body = accent rim drawn a touch larger, steel gradient opaque on top, then a
// highlight crescent. Everything here is drawn opaque into the compose layer,
// so the rim really is a rim -- the body covers it except at the edge -- and
// overlapping pieces of a pill do not double-blend into visible seams.
static void DrawBody(SDL_Renderer *r, int i, float w, float h, boolean lit)
{
    float cx = btn[i].cx * w;
    float cy = btn[i].cy * h;
    float rr = btn[i].r * h;
    Uint8 rr_, rg_, rb_;

    if (lit)
    {
        rr_ = 255; rg_ = 255; rb_ = 255;
    }
    else
    {
        rr_ = ACC_R; rg_ = ACC_G; rb_ = ACC_B;
    }

    // A dark halo first. DOOM's walls can be brighter than the controls, and a
    // thin rim over a bright wall disappears -- the pills lose that fight worst
    // because they have far less body mass than the discs. The halo gives every
    // control separation from whatever is behind it.
    if (btn[i].pill)
    {
        float hw = btn[i].hw * h;

        DrawPill(r, false, cx, cy, hw + 9.0f, rr + 9.0f, 0, 0, 0, 110);
        DrawPill(r, false, cx, cy, hw + 3.0f, rr + 3.0f, rr_, rg_, rb_, 255);
        DrawPill(r, true,  cx, cy, hw, rr, STEEL_R, STEEL_G, STEEL_B, 255);
    }
    else
    {
        DrawSq(r, tex_disc_f, cx, cy, rr + 9.0f, 0, 0, 0, 110);
        DrawSq(r, tex_disc_f, cx, cy, rr + 3.0f, rr_, rg_, rb_, 255);
        DrawSq(r, tex_disc_g, cx, cy, rr, STEEL_R, STEEL_G, STEEL_B, 255);
        DrawSq(r, tex_hl, cx, cy, rr, 255, 255, 255, 105);
    }
}

void AT_DrawGhost(SDL_Renderer *r)
{
    int      w, h, i, lw, lh;
    SDL_bool iscale;

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
    // 320x200-lineage framebuffer. Real panel pixels are the point.
    SDL_RenderGetLogicalSize(r, &lw, &lh);
    iscale = SDL_RenderGetIntegerScale(r);

    if (lw > 0 && lh > 0)
    {
        SDL_RenderSetLogicalSize(r, 0, 0);
    }

    cur_aspect = (float) w / (float) h;
    Layout();
    EnsureTextures(r);

    if (tex_layer == NULL || layer_w != w || layer_h != h)
    {
        if (tex_layer != NULL)
        {
            SDL_DestroyTexture(tex_layer);
        }
        tex_layer = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_TARGET, w, h);
        layer_w = w;
        layer_h = h;

        if (tex_layer != NULL)
        {
            SDL_SetTextureBlendMode(tex_layer, SDL_BLENDMODE_BLEND);
        }
    }

    if (tex_layer != NULL)
    {
        SDL_Texture *prev = SDL_GetRenderTarget(r);

        // Compose the whole set opaque into its own layer, then blit that once
        // at the user's opacity. Blending each piece straight onto the frame
        // double-blends every overlap -- pill caps seam against their bar, and
        // an under-drawn rim shows through the translucent body instead of
        // staying a rim. Composing first makes the opacity uniform too.
        SDL_SetRenderTarget(r, tex_layer);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
        SDL_RenderClear(r);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

        if (!menuactive)
        {
            float dcx = DPAD_CX * w;
            float dcy = DPAD_CY * h;
            float da  = DPAD_ARM * h;
            float ncx = NUB_CX * w;
            float ncy = NUB_CY * h;
            float cpx = ncx + nub_dx * h;
            float cpy = ncy + nub_dy * h;

            float ch = da * 0.19f;   // chevron size
            float co = da * 0.62f;   // chevron offset from centre

            DrawSq(r, tex_dpad_f, dcx, dcy, da + 9.0f, 0, 0, 0, 110);
            DrawSq(r, tex_dpad_f, dcx, dcy, da + 3.0f, ACC_R, ACC_G, ACC_B, 255);
            DrawSq(r, tex_dpad_g, dcx, dcy, da, STEEL_R, STEEL_G, STEEL_B, 255);

            // Direction chevrons. Without them the cross is just a shape --
            // nothing tells a thumb it is a directional control.
            DrawSqRot(r, tex_tri_s, dcx, dcy - co, ch,   0.0, ACC_R, ACC_G, ACC_B, 200);
            DrawSqRot(r, tex_tri_s, dcx, dcy + co, ch, 180.0, ACC_R, ACC_G, ACC_B, 200);
            DrawSqRot(r, tex_tri_s, dcx - co, dcy, ch, 270.0, ACC_R, ACC_G, ACC_B, 200);
            DrawSqRot(r, tex_tri_s, dcx + co, dcy, ch,  90.0, ACC_R, ACC_G, ACC_B, 200);

            // Concave dish, accent track, convex cap: the inverted gradient on
            // the dish is what sells the cap as sitting inside it.
            DrawSq(r, tex_disc_f, ncx, ncy, NUB_TRACK * h + 9.0f, 0, 0, 0, 110);
            DrawSq(r, tex_dish, ncx, ncy, NUB_TRACK * h,
                   STEEL_R, STEEL_G, STEEL_B, 255);
            DrawSq(r, tex_track, ncx, ncy, NUB_TRACK * h,
                   ACC_R, ACC_G, ACC_B, 255);
            DrawSq(r, tex_disc_f, cpx, cpy, NUB_CAP * h + 3.0f,
                   ACC_R, ACC_G, ACC_B, 255);
            DrawSq(r, tex_disc_g, cpx, cpy, NUB_CAP * h,
                   STEEL_R, STEEL_G, STEEL_B, 255);
            DrawSq(r, tex_hl, cpx, cpy, NUB_CAP * h, 255, 255, 255, 105);
        }

        for (i = 0; i < NBTN; i++)
        {
            float cx, cy, rr;
            Uint8 sr = 255, sg = 255, sb = 255;

            if (menuactive && i != B_START)
            {
                continue;
            }

            DrawBody(r, i, (float) w, (float) h, btn[i].down);

            cx = btn[i].cx * w;
            cy = btn[i].cy * h;
            rr = btn[i].r * h;

            switch (i)
            {
                case B_FIRE: sr = 240; sg =  82; sb =  79; break;
                case B_USE:  sr =  33; sg = 150; sb = 243; break;
                case B_NEXT: sr =  95; sg = 208; sb = 138; break;
                case B_PREV: sr = 233; sg =  58; sb = 142; break;
                default: break;
            }

            // A pressed button blooms to white rather than merely brightening,
            // so a thumb still gets feedback at a low opacity setting.
            if (btn[i].down)
            {
                sr = 255; sg = 255; sb = 255;
            }

            switch (i)
            {
                case B_FIRE:  DrawSq(r, tex_o,   cx, cy, rr, sr, sg, sb, 255); break;
                case B_USE:   DrawSq(r, tex_x,   cx, cy, rr, sr, sg, sb, 255); break;
                case B_NEXT:  DrawSq(r, tex_tri, cx, cy, rr, sr, sg, sb, 255); break;
                case B_PREV:  DrawSq(r, tex_sq,  cx, cy, rr, sr, sg, sb, 255); break;
                case B_L:     DrawLabel(r, LBL_L, cx, cy, PILL_TEXT_H * h, 255); break;
                case B_R:     DrawLabel(r, LBL_R, cx, cy, PILL_TEXT_H * h, 255); break;
                case B_START: DrawLabel(r, LBL_START, cx, cy, PILL_TEXT_H * h, 255); break;
                default:      DrawLabel(r, LBL_SELECT, cx, cy, PILL_TEXT_H * h, 255); break;
            }
        }

        SDL_SetRenderTarget(r, prev);
        SDL_SetTextureAlphaMod(tex_layer, (Uint8) ghost_alpha);
        SDL_RenderCopy(r, tex_layer, NULL, NULL);
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
                btn[b].down = true;
                btn[b].finger = ev->tfinger.fingerId;
                PostKey(ev_keydown, ButtonKey(b));
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

            if (ghost_on && !dpad_on && HitDpad(x, y))
            {
                dpad_on = true;
                dpad_id = ev->tfinger.fingerId;
                ApplyDpad(x, y);
                return;
            }

            if (ghost_on && !nub_on && HitNub(x, y))
            {
                nub_on = true;
                nub_id = ev->tfinger.fingerId;
                ApplyNub(x, y);
                return;
            }

            if (!look_on)
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

            if (dpad_on && ev->tfinger.fingerId == dpad_id)
            {
                ApplyDpad(x, y);
            }
            else if (nub_on && ev->tfinger.fingerId == nub_id)
            {
                ApplyNub(x, y);
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

            if (dpad_on && ev->tfinger.fingerId == dpad_id)
            {
                dpad_on = false;
                ReleaseMove();
                return;
            }

            if (nub_on && ev->tfinger.fingerId == nub_id)
            {
                nub_on = false;
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
