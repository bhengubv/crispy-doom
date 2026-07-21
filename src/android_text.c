//
// Native-resolution text for Crispy Doom on Android / Circle OS.
//
// DOOM rasterises every string into the 320x200-lineage framebuffer, which is
// then magnified about 5.4x to reach a phone panel. That is the whole reason
// the menus and the HUD arrive soft: the detail was never in the source, so no
// scaler can put it back. Dimming the background and changing the upscaler both
// failed for the same reason.
//
// Rather than re-implement each screen, this intercepts the only three places
// the engine actually draws a character -- M_WriteText, HUlib_drawTextLine and
// F_TextWrite -- and records the glyph instead of blitting it. i_video.c then
// replays the queue over the finished frame, drawing from the Arcade's own
// ui_font.ttf at true panel resolution.
//
// The engine still decides WHAT text goes WHERE. Positions, line breaks,
// centring, colour and wrapping are all computed by the game against the
// vanilla font metrics, exactly as before; only the rasterisation changes. A
// run is laid out to fill precisely the box the engine measured, so a string
// DOOM centred is still centred and one it left-aligned still starts on its
// margin. That is deliberate -- the last attempt replaced the menu's layout as
// well as its font, and the menus stopped working the way they were designed.
//
// Not covered, because they are artwork rather than a font: the status bar
// numerals, the intermission tallies, and the menu title/banner lumps. Those
// are drawn from their own patches and would have to be redrawn, not restyled.
//

#include <string.h>
#include <stdlib.h>

#include "SDL.h"

#include "doomtype.h"
#include "i_video.h"
#include "v_trans.h"

#include "android_text.h"
#include "android_glyphs.h"

#define AX_MAX_RUNS    256
#define AX_MAX_CHARS   4096
#define AX_MAX_THERMOS 8

// The Arcade accent, the one blue the whole ecosystem uses.
#define ACC_R  33
#define ACC_G 150
#define ACC_B 243

// A run is a stretch of characters the engine laid down back to back on one
// line in one colour. Keeping them together is what lets the layout fill the
// engine's own box instead of guessing at letter spacing.
typedef struct
{
    short x, y;         // 320x200-space origin of the run's first cell
    short w;            // total width of its cells, same space
    short h;            // vanilla glyph height -- the cap height to match
    short first, len;   // slice of ax_chars
    Uint8 r, g, b;
} ax_run_t;

typedef struct
{
    short x, y, w, h;
    float frac;
} ax_thermo_t;

static ax_run_t    ax_runs[AX_MAX_RUNS];
static char        ax_chars[AX_MAX_CHARS];
static ax_thermo_t ax_thermos[AX_MAX_THERMOS];
static int         ax_nruns;
static int         ax_nchars;
static int         ax_nthermos;

static SDL_Texture *atlas;
static boolean      atlas_tried;

// ---------------------------------------------------------------------------
// glyph atlas
// ---------------------------------------------------------------------------

static void EnsureAtlas(SDL_Renderer *r)
{
    Uint32 *px;
    int     i, o, n;

    if (atlas_tried)
    {
        return;
    }
    atlas_tried = true;

    n = GLYPH_ATLAS_W * GLYPH_ATLAS_H;
    px = malloc(n * sizeof(Uint32));

    if (px == NULL)
    {
        return;
    }

    // The atlas ships run-length encoded as coverage only. Expand it to white
    // pixels whose alpha is that coverage, so the colour comes from the tint
    // and one texture serves every colour the engine asks for.
    o = 0;
    for (i = 0; i + 1 < GLYPH_RLE_LEN && o < n; i += 2)
    {
        int    run = glyph_rle[i];
        Uint8  cov = glyph_rle[i + 1];
        Uint32 v = ((Uint32) cov << 24) | 0x00ffffffu;
        int    k;

        for (k = 0; k < run && o < n; k++)
        {
            px[o++] = v;
        }
    }
    while (o < n)
    {
        px[o++] = 0;
    }

    atlas = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STATIC,
                              GLYPH_ATLAS_W, GLYPH_ATLAS_H);

    if (atlas != NULL)
    {
        SDL_UpdateTexture(atlas, NULL, px, GLYPH_ATLAS_W * sizeof(Uint32));
        SDL_SetTextureBlendMode(atlas, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(atlas, SDL_ScaleModeLinear);
    }

    free(px);
}

static const glyph_t *FindGlyph(char c)
{
    int i;

    for (i = 0; i < GLYPH_COUNT; i++)
    {
        if (glyph_tab[i].ch == c)
        {
            return &glyph_tab[i];
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// colour
// ---------------------------------------------------------------------------

// crispy carries text colour as a palette translation table. Map the tables we
// know to Arcade-side colours; anything unrecognised falls back to plain white,
// which is the same thing an untranslated string gets.
static void ColorFor(const byte *trans, Uint8 *r, Uint8 *g, Uint8 *b)
{
    static const struct { int idx; Uint8 r, g, b; } map[] = {
        { CR_DARK,      110, 110, 110 },
        { CR_DIMMED,    110, 110, 110 },
        { CR_GRAY,      205, 205, 205 },
        { CR_GREEN,     110, 220, 120 },
        { CR_GOLD,      235, 195,  95 },
        { CR_RED,       235,  95,  85 },
        { CR_BLUE,    ACC_R, ACC_G, ACC_B },
        { CR_RED2BLUE, ACC_R, ACC_G, ACC_B },
        { CR_RED2GREEN, 110, 220, 120 },
    };
    size_t i;

    *r = *g = *b = 255;

    if (trans == NULL)
    {
        return;
    }

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    {
        if (map[i].idx < CRMAX && trans == cr[map[i].idx])
        {
            *r = map[i].r;
            *g = map[i].g;
            *b = map[i].b;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// queue
// ---------------------------------------------------------------------------

boolean AX_Glyph(int x, int y, int cellw, int cellh, char ch,
                 const byte *trans)
{
    ax_run_t *run;
    Uint8     r, g, b;

    if (cellw <= 0 || cellh <= 0 || FindGlyph(ch) == NULL)
    {
        return false;   // let the engine draw it the old way
    }

    if (ax_nchars >= AX_MAX_CHARS)
    {
        return false;
    }

    ColorFor(trans, &r, &g, &b);

    // Continue the previous run when this cell follows it on the same line in
    // the same colour. A small gap is absorbed rather than ending the run: the
    // engine skips spaces without drawing them, and a string that got split at
    // every space could not be recognised as centred later on.
    run = ax_nruns > 0 ? &ax_runs[ax_nruns - 1] : NULL;

    if (run != NULL
     && run->y == (short) y
     && run->h == (short) cellh
     && run->r == r && run->g == g && run->b == b
     && run->first + run->len == ax_nchars)
    {
        int gap = x - (run->x + run->w);

        if (gap > 0 && gap <= 12 && ax_nchars < AX_MAX_CHARS - 1)
        {
            ax_chars[ax_nchars++] = ' ';
            run->len++;
            run->w += (short) gap;
        }
    }

    if (run == NULL
     || run->y != (short) y
     || run->h != (short) cellh
     || run->r != r || run->g != g || run->b != b
     || run->x + run->w != (short) x
     || run->first + run->len != ax_nchars)
    {
        if (ax_nruns >= AX_MAX_RUNS)
        {
            return false;
        }

        run = &ax_runs[ax_nruns++];
        run->x = (short) x;
        run->y = (short) y;
        run->w = 0;
        run->h = (short) cellh;
        run->first = (short) ax_nchars;
        run->len = 0;
        run->r = r;
        run->g = g;
        run->b = b;
    }

    ax_chars[ax_nchars++] = ch;
    run->len++;
    run->w += (short) cellw;

    return true;
}

boolean AX_Thermo(int x, int y, int w, int h, float frac)
{
    ax_thermo_t *t;

    if (ax_nthermos >= AX_MAX_THERMOS || w <= 0 || h <= 0)
    {
        return false;
    }

    t = &ax_thermos[ax_nthermos++];
    t->x = (short) x;
    t->y = (short) y;
    t->w = (short) w;
    t->h = (short) h;
    t->frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);

    return true;
}

// ---------------------------------------------------------------------------
// draw
// ---------------------------------------------------------------------------

// Where the game frame lands in output pixels. crispy hands the renderer a
// logical size so the frame is scaled and letterboxed; this reproduces that
// placement so queued text can be positioned against the frame rather than the
// panel, and stays aligned whatever the aspect settings are doing.
static void FrameRect(SDL_Renderer *r, SDL_FRect *out)
{
    int   lw, lh, ow, oh;
    float s;

    SDL_RenderGetLogicalSize(r, &lw, &lh);
    SDL_GetRendererOutputSize(r, &ow, &oh);

    if (lw <= 0 || lh <= 0)
    {
        out->x = 0.0f;
        out->y = 0.0f;
        out->w = (float) ow;
        out->h = (float) oh;
        return;
    }

    s = SDL_min((float) ow / (float) lw, (float) oh / (float) lh);

    if (SDL_RenderGetIntegerScale(r))
    {
        s = SDL_floorf(s);
        if (s < 1.0f)
        {
            s = 1.0f;
        }
    }

    out->w = lw * s;
    out->h = lh * s;
    out->x = (ow - out->w) * 0.5f;
    out->y = (oh - out->h) * 0.5f;
}

static void DrawRun(SDL_Renderer *rend, const ax_run_t *run,
                    const SDL_FRect *fr, float ox, float oy, Uint8 alpha)
{
    const float origw = (float)(ORIGWIDTH + 2 * WIDESCREENDELTA);
    float px = fr->w / origw;               // output pixels per 320-space unit
    float py = fr->h / (float) ORIGHEIGHT;
    float x0 = fr->x + (run->x + WIDESCREENDELTA) * px;
    float boxw = run->w * px;
    float scale, natural, adv, pen, top;
    int   i;

    // Match the vanilla cap height, so text sits on the same line the engine
    // laid out and keeps the weight the screen was designed around.
    scale = (run->h * py) / (float) GLYPH_CAPHEIGHT;

    natural = 0.0f;
    for (i = 0; i < run->len; i++)
    {
        const glyph_t *gl = FindGlyph(ax_chars[run->first + i]);
        if (gl != NULL)
        {
            natural += gl->adv * scale;
        }
    }

    // Letter spacing stays natural. The engine measured this string with DOOM's
    // all-caps font, so a mixed-case run is genuinely narrower than the box it
    // was given -- stretching to fill that box spaces the word out until it
    // reads as a mistake. Only squeeze, and only when we would overflow.
    adv = (natural > boxw && natural > 0.5f) ? (boxw / natural) : 1.0f;

    // Being narrower than the box is fine for text the engine left-aligned, but
    // wrong for text it centred, which would sit off to the right by half the
    // slack. There is no alignment flag to read, so recognise the one case that
    // matters: a box centred on the screen's midline was centred deliberately.
    if (natural < boxw
     && abs((run->x + run->w / 2) - ORIGWIDTH / 2) <= 2)
    {
        x0 += (boxw - natural) * 0.5f;
    }

    top = fr->y + (run->y + run->h) * py - GLYPH_ASCENT * scale;

    SDL_SetTextureColorMod(atlas, run->r, run->g, run->b);
    SDL_SetTextureAlphaMod(atlas, alpha);

    pen = x0;
    for (i = 0; i < run->len; i++)
    {
        const glyph_t *gl = FindGlyph(ax_chars[run->first + i]);
        SDL_Rect  src;
        SDL_FRect dst;

        if (gl == NULL)
        {
            continue;
        }

        if (gl->w > 0)
        {
            src.x = gl->x;
            src.y = 0;
            src.w = gl->w;
            src.h = GLYPH_ATLAS_H;

            dst.x = pen + ox;
            dst.y = top + oy;
            dst.w = gl->w * scale;
            dst.h = GLYPH_ATLAS_H * scale;

            SDL_RenderCopyF(rend, atlas, &src, &dst);
        }

        pen += gl->adv * scale * adv;
    }
}

void AX_Draw(SDL_Renderer *r)
{
    SDL_FRect fr;
    int       lw, lh, i;
    SDL_bool  iscale;
    float     drop;

    if (r == NULL || (ax_nruns == 0 && ax_nthermos == 0))
    {
        ax_nruns = ax_nchars = ax_nthermos = 0;
        return;
    }

    EnsureAtlas(r);

    if (atlas == NULL)
    {
        ax_nruns = ax_nchars = ax_nthermos = 0;
        return;
    }

    FrameRect(r, &fr);

    // Drop the logical size while drawing. Left on, the text would be laid out
    // in panel pixels but drawn through the logical transform, resampling it
    // back down to the very resolution this exists to escape.
    SDL_RenderGetLogicalSize(r, &lw, &lh);
    iscale = SDL_RenderGetIntegerScale(r);

    if (lw > 0 && lh > 0)
    {
        SDL_RenderSetLogicalSize(r, 0, 0);
    }

    // Half a 320x200 pixel. Enough to hold an edge against a bright wall or the
    // Crispness menu's tiled flat, small enough that a dimmed grey row does not
    // read as a second, offset copy of itself.
    drop = fr.h / (float) ORIGHEIGHT * 0.5f;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    for (i = 0; i < ax_nthermos; i++)
    {
        const ax_thermo_t *t = &ax_thermos[i];
        float px = fr.w / (float)(ORIGWIDTH + 2 * WIDESCREENDELTA);
        float py = fr.h / (float) ORIGHEIGHT;
        float bar = t->h * py * 0.34f;
        SDL_FRect track, fill, knob;

        track.x = fr.x + (t->x + WIDESCREENDELTA) * px;
        track.w = t->w * px;
        track.y = fr.y + t->y * py + (t->h * py - bar) * 0.5f;
        track.h = bar;

        fill = track;
        fill.w = track.w * t->frac;

        // Knob sized off the bar rather than the track, so it stays a knob at
        // any panel size instead of a square that grows with the slider.
        knob.w = bar * 2.2f;
        knob.h = bar * 2.2f;
        knob.x = track.x + fill.w - knob.w * 0.5f;
        knob.y = track.y + (bar - knob.h) * 0.5f;

        SDL_SetRenderDrawColor(r, 255, 255, 255, 46);
        SDL_RenderFillRectF(r, &track);
        SDL_SetRenderDrawColor(r, ACC_R, ACC_G, ACC_B, 220);
        SDL_RenderFillRectF(r, &fill);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 235);
        SDL_RenderFillRectF(r, &knob);
    }

    for (i = 0; i < ax_nruns; i++)
    {
        ax_run_t shadow = ax_runs[i];

        // A soft drop shadow instead of a panel behind the text. The menus sit
        // over live gameplay, and a scrim wide enough to guarantee contrast
        // covers the game; a shadow buys the same legibility over a bright wall
        // without hiding what is behind it.
        shadow.r = shadow.g = shadow.b = 0;
        DrawRun(r, &shadow, &fr, drop, drop, 170);

        DrawRun(r, &ax_runs[i], &fr, 0.0f, 0.0f, 255);
    }

    if (lw > 0 && lh > 0)
    {
        SDL_RenderSetLogicalSize(r, lw, lh);
        SDL_RenderSetIntegerScale(r, iscale);
    }

    ax_nruns = ax_nchars = ax_nthermos = 0;
}
