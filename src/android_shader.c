//
// GPU edge-directed upscale for Crispy Doom on Android / Circle OS.
//
// crispy already does sharp-bilinear: it renders the game to a texture, blows
// that up to an integer multiple with nearest sampling, then lets the GPU take
// it the rest of the way with a linear filter. Good, but not edge aware -- a
// diagonal still arrives as either a staircase or a smear.
//
// This does the last step with a shader instead. For each output pixel it looks
// at the 2x2 source texels around it and compares the two diagonals. Where one
// diagonal is much flatter than the other there is an edge running along it, so
// it interpolates ALONG that edge rather than across it. Flat regions fall back
// to plain bilinear. The result keeps diagonals smooth without the mush a wider
// blur gives you.
//
// SDL_Renderer exposes no shader hook, so this drops to raw GLES2 against SDL's
// own context: SDL_RenderFlush() to drain SDL's queued work, then our own pass.
//
// IMPORTANT -- interop boundary. Once this has issued raw GL to the default
// framebuffer, SDL_Renderer's cached GL state is stale, and neither a later
// SDL_RenderCopy nor a texture sampled inside this pass will reliably reach the
// screen on this device (measured on a Mali-G52 / Helio G85: the SDL draw drew
// nothing, and a render-target texture sampled on the default framebuffer read
// back zero while the identical read into a fresh FBO returned the real
// pixels). So this pass must be the WHOLE frame or none of it. It therefore
// only runs when there is nothing else to draw over the top -- see the gate in
// i_video: the ghost controls have to be on the stock SDL path, so the shader
// engages only when the controls are switched off.
//
// Everything here also fails safe. If the context is not GLES2, or a shader
// will not compile, it disables itself for good and reports failure so the
// caller uses crispy's stock path. A cosmetic upgrade must never cost a working
// build.
//

#include <stdlib.h>
#include <string.h>

#include "SDL.h"
#include "SDL_opengles2.h"

#include "doomtype.h"
#include "m_argv.h"
#include "android_shader.h"

static const char *VERT_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_tc;\n"
    "varying vec2 v_tc;\n"
    "void main() {\n"
    "    v_tc = a_tc;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *FRAG_SRC =
    "precision highp float;\n"
    "varying vec2 v_tc;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_src;\n"       // source size in texels
    "uniform vec2 u_scale;\n"     // SDL texcoord scale (NPOT padding)
    "uniform float u_swap;\n"     // 1.0 when the texture is BGRA in memory
    "\n"
    "float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
    "\n"
    // SDL_PIXELFORMAT_ARGB8888 is B,G,R,A in memory on little-endian and SDL
    // hands that to GL as plain RGBA bytes, so a raw sample gives BGR -- every
    // red comes out blue. Which way round it is is queried, not assumed.
    "vec3 tap(vec2 uv) {\n"
    "    vec3 c = texture2D(u_tex, uv * u_scale).rgb;\n"
    "    return mix(c, c.bgr, u_swap);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec2 texel = 1.0 / u_src;\n"
    "    vec2 pos   = v_tc * u_src - 0.5;\n"
    "    vec2 f     = fract(pos);\n"
    "    vec2 base  = (floor(pos) + 0.5) * texel;\n"
    "\n"
    "    vec3 A = tap(base);\n"                                  // top-left
    "    vec3 B = tap(base + vec2(texel.x, 0.0));\n"             // top-right
    "    vec3 C = tap(base + vec2(0.0, texel.y));\n"             // bottom-left
    "    vec3 D = tap(base + texel);\n"                          // bottom-right
    "\n"
    "    vec3 top = mix(A, B, f.x);\n"
    "    vec3 bot = mix(C, D, f.x);\n"
    "    vec3 bilinear = mix(top, bot, f.y);\n"
    "\n"
    // Which diagonal is the flat one? The flatter diagonal is the edge.
    "    float dAD = abs(luma(A) - luma(D));\n"
    "    float dBC = abs(luma(B) - luma(C));\n"
    "    float conf = abs(dAD - dBC) / (dAD + dBC + 0.0001);\n"
    "\n"
    "    vec3 diag;\n"
    "    if (dAD < dBC) {\n"
    "        diag = mix(A, D, clamp((f.x + f.y) * 0.5, 0.0, 1.0));\n"
    "    } else {\n"
    "        diag = mix(C, B, clamp((f.x + (1.0 - f.y)) * 0.5, 0.0, 1.0));\n"
    "    }\n"
    "\n"
    // Commit to the diagonal only where the evidence is strong; smoothstep
    // keeps flat areas on honest bilinear instead of inventing structure.
    "    float w = smoothstep(0.18, 0.75, conf);\n"
    "    gl_FragColor = vec4(mix(bilinear, diag, w), 1.0);\n"
    "}\n";

static int      enabled = -1;   // -1 = not yet read from argv
static boolean  tried;
static boolean  ok;
static GLuint   prog, vbo;
static GLint    a_pos, a_tc, u_tex, u_src, u_scale, u_swap;

static void ReadArgs(void)
{
    int p;

    if (enabled >= 0)
    {
        return;
    }
    enabled = 1;

    //!
    // @arg <0|1>
    //
    // Edge-directed GPU upscale. Passed by the Circle OS Arcade. Note the
    // upscale only takes effect while the on-screen controls are off.
    //
    p = M_CheckParmWithArgs("-shader", 1);

    if (p > 0)
    {
        enabled = atoi(myargv[p + 1]) ? 1 : 0;
    }
}

static GLuint Compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint  status = 0;

    if (s == 0)
    {
        return 0;
    }

    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &status);

    if (!status)
    {
        char log[512];
        GLsizei n = 0;

        glGetShaderInfoLog(s, sizeof(log) - 1, &n, log);
        log[n] = '\0';
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "AS: shader compile failed: %s",
                     log);
        glDeleteShader(s);
        return 0;
    }

    return s;
}

static boolean Build(void)
{
    GLuint vs, fs;
    GLint  status = 0;
    static const GLfloat quad[] = {
        // x, y,   u, v   -- v is flipped: SDL textures are top-left origin,
        // the default framebuffer is bottom-left.
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
    };

    vs = Compile(GL_VERTEX_SHADER, VERT_SRC);

    if (vs == 0)
    {
        return false;
    }

    fs = Compile(GL_FRAGMENT_SHADER, FRAG_SRC);

    if (fs == 0)
    {
        glDeleteShader(vs);
        return false;
    }

    prog = glCreateProgram();

    if (prog == 0)
    {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!status)
    {
        char log[512];
        GLsizei n = 0;

        glGetProgramInfoLog(prog, sizeof(log) - 1, &n, log);
        log[n] = '\0';
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "AS: link failed: %s", log);
        glDeleteProgram(prog);
        prog = 0;
        return false;
    }

    a_pos   = glGetAttribLocation(prog, "a_pos");
    a_tc    = glGetAttribLocation(prog, "a_tc");
    u_tex   = glGetUniformLocation(prog, "u_tex");
    u_src   = glGetUniformLocation(prog, "u_src");
    u_scale = glGetUniformLocation(prog, "u_scale");
    u_swap  = glGetUniformLocation(prog, "u_swap");

    // Only the attributes and the sampler are genuinely required. A uniform the
    // shader does not reference is legally optimised out and reports -1, and
    // glUniform* on -1 is a defined no-op -- demanding all of them would turn a
    // normal compiler optimisation into a silent, total failure of the feature.
    if (a_pos < 0 || a_tc < 0 || u_tex < 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                     "AS: missing required attrib/sampler "
                     "(a_pos=%d a_tc=%d u_tex=%d)", a_pos, a_tc, u_tex);
        glDeleteProgram(prog);
        prog = 0;
        return false;
    }

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "AS: edge-directed upscale ready");

    return true;
}

boolean AS_Enabled(void)
{
    ReadArgs();
    return enabled == 1;
}

boolean AS_Present(SDL_Renderer *r, SDL_Texture *src, int srcw, int srch)
{
    SDL_RendererInfo info;
    int   outw = 0, outh = 0;
    float tw = 1.0f, th = 1.0f;

    ReadArgs();

    if (!enabled || r == NULL || src == NULL || srcw <= 0 || srch <= 0)
    {
        return false;
    }

    if (!tried)
    {
        tried = true;

        // Only the GLES2 backend gives us a context we can issue our own
        // commands against. Anything else keeps the stock path.
        if (SDL_GetRendererInfo(r, &info) != 0 ||
            info.name == NULL ||
            strstr(info.name, "opengles2") == NULL)
        {
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                        "AS: renderer is '%s', not opengles2 -- staying off",
                        (info.name != NULL) ? info.name : "?");
            ok = false;
        }
        else
        {
            ok = Build();
        }
    }

    if (!ok)
    {
        return false;
    }

    if (SDL_GetRendererOutputSize(r, &outw, &outh) != 0 ||
        outw <= 0 || outh <= 0)
    {
        return false;
    }

    // Drain whatever SDL still has queued, so our pass lands in order and SDL
    // is not mid-batch when we start changing GL state.
    SDL_RenderFlush(r);

    glActiveTexture(GL_TEXTURE0);

    if (SDL_GL_BindTexture(src, &tw, &th) != 0)
    {
        return false;
    }

    // Nearest on the source: the shader does its own reconstruction, and a
    // driver-side linear filter underneath would blur the very texels it reads.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    {
        Uint32 fmt = 0;

        SDL_QueryTexture(src, &fmt, NULL, NULL, NULL);
        glUseProgram(prog);
        glUniform1i(u_tex, 0);
        glUniform2f(u_src, (float) srcw, (float) srch);
        glUniform2f(u_scale, tw, th);
        glUniform1f(u_swap,
                    (fmt == SDL_PIXELFORMAT_ARGB8888 ||
                     fmt == SDL_PIXELFORMAT_XRGB8888) ? 1.0f : 0.0f);
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(a_pos);
    glEnableVertexAttribArray(a_tc);
    glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), (const void *) 0);
    glVertexAttribPointer(a_tc, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat),
                          (const void *)(2 * sizeof(GLfloat)));

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, outw, outh);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(a_pos);
    glDisableVertexAttribArray(a_tc);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);

    SDL_GL_UnbindTexture(src);

    return true;
}
