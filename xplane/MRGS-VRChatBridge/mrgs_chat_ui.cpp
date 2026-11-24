/* ============================================================================
 * MrGameSimulation VR Chat Bridge - X-Plane 12 window (v1.4.1)
 *
 * A standalone Social Stream Ninja chat window for X-Plane 12, on
 * the desktop and in VR. Any streamer can use it: install the
 * plugin, paste your own Social Stream Ninja session ID into the
 * local setup page, and your chat (Twitch, YouTube, Kick, TikTok -
 * whatever Social Stream Ninja is connected to) lands in one clean
 * in-sim window.
 *
 * This file is the DISPLAY only. The companion Python engine
 * (PI_MRGSChatBridge.py, running under XPPython3) owns the network:
 * it joins the SSN channel-4 relay, keeps the newest 40 messages,
 * and publishes them to PythonPlugins/mrgs_chat_state.json, which
 * this window polls (~4 Hz). Intents (Clear) are appended to
 * mrgs_chat_actions.jsonl. Rendering is Dear ImGui on raw OpenGL,
 * drawn inside a single self-decorated XPLM window - the same
 * architecture proven by the MRGS VR Chat window.
 *
 * The window background has an adjustable opacity (footer slider,
 * 0-100%): at 0% the panel disappears and chat text floats over
 * the cockpit - made for VR. Text and controls always stay fully
 * opaque. The setting persists in mrgs_chat_ui_prefs.txt.
 *
 * Build (macOS arm64):
 *   zig c++ -target aarch64-macos -shared -O2 -DAPL=1 -DXPLM200 \
 *     -DXPLM210 -DXPLM300 -DXPLM301 -DXPLM303 -DXPLM400 -DXPLM420 \
 *     -I <SDK>/CHeaders/XPLM -I <SDK>/CHeaders/Widgets -I <imgui> \
 *     mrgs_chat_ui.cpp <imgui>/imgui.cpp <imgui>/imgui_draw.cpp \
 *     <imgui>/imgui_widgets.cpp <imgui>/imgui_tables.cpp \
 *     -Wl,-undefined,dynamic_lookup -o mac.xpl
 * ==========================================================================*/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "XPLMDefs.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMCamera.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMMenus.h"
#include "XPLMUtilities.h"
#include "XPLMDataAccess.h"

#include "imgui.h"

static XPLMWindowID g_win = NULL;

/* ---------------- GL: types, constants, dlsym-resolved pointers ------- */

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef char GLchar;
typedef long GLsizeiptr;
typedef long GLintptr;

#define GL_FALSE                 0
#define GL_TRUE                  1
#define GL_TRIANGLES             0x0004
#define GL_UNSIGNED_SHORT        0x1403
#define GL_UNSIGNED_INT          0x1405
#define GL_FLOAT                 0x1406
#define GL_VIEWPORT              0x0BA2
#define GL_MODELVIEW_MATRIX      0x0BA6
#define GL_PROJECTION_MATRIX     0x0BA7
#define GL_RGBA                  0x1908
#define GL_UNSIGNED_BYTE         0x1401
#define GL_TEXTURE_2D            0x0DE1
#define GL_TEXTURE_MAG_FILTER    0x2800
#define GL_TEXTURE_MIN_FILTER    0x2801
#define GL_TEXTURE_WRAP_S        0x2802
#define GL_TEXTURE_WRAP_T        0x2803
#define GL_LINEAR                0x2601
#define GL_CLAMP_TO_EDGE         0x812F
#define GL_ARRAY_BUFFER          0x8892
#define GL_ELEMENT_ARRAY_BUFFER  0x8893
#define GL_STREAM_DRAW           0x88E0
#define GL_BLEND                 0x0BE2
#define GL_SRC_ALPHA             0x0302
#define GL_ONE_MINUS_SRC_ALPHA   0x0303
#define GL_FUNC_ADD              0x8006
#define GL_SCISSOR_TEST          0x0C11
#define GL_CULL_FACE             0x0B44
#define GL_DEPTH_TEST            0x0B71
#define GL_VERTEX_SHADER         0x8B31
#define GL_FRAGMENT_SHADER       0x8B30
#define GL_COMPILE_STATUS        0x8B81
#define GL_LINK_STATUS           0x8B82
#define GL_ACTIVE_TEXTURE        0x84E0
#define GL_TEXTURE0              0x84C0
#define GL_VENDOR                0x1F00
#define GL_VERSION               0x1F02

typedef const GLubyte *(*PFN_glGetString)(GLenum);
typedef void (*PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (*PFN_glBindTexture)(GLenum, GLuint);
typedef void (*PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (*PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei,
                                 GLint, GLenum, GLenum, const void *);
typedef void (*PFN_glActiveTexture)(GLenum);
typedef void (*PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void (*PFN_glBindBuffer)(GLenum, GLuint);
typedef void (*PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (*PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void (*PFN_glBindVertexArray)(GLuint);
typedef void (*PFN_glVertexAttribPointer)(GLuint, GLint, GLenum,
                                          GLubyte, GLsizei, const void *);
typedef void (*PFN_glEnableVertexAttribArray)(GLuint);
typedef GLuint (*PFN_glCreateShader)(GLenum);
typedef void (*PFN_glShaderSource)(GLuint, GLsizei, const GLchar *const *,
                                   const GLint *);
typedef void (*PFN_glCompileShader)(GLuint);
typedef void (*PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void (*PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint (*PFN_glCreateProgram)(void);
typedef void (*PFN_glAttachShader)(GLuint, GLuint);
typedef void (*PFN_glBindAttribLocation)(GLuint, GLuint, const GLchar *);
typedef void (*PFN_glLinkProgram)(GLuint);
typedef void (*PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void (*PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *,
                                        GLchar *);
typedef void (*PFN_glUseProgram)(GLuint);
typedef GLint (*PFN_glGetUniformLocation)(GLuint, const GLchar *);
typedef void (*PFN_glUniform1i)(GLint, GLint);
typedef void (*PFN_glUniformMatrix4fv)(GLint, GLsizei, GLubyte,
                                       const GLfloat *);
typedef void (*PFN_glEnable)(GLenum);
typedef void (*PFN_glDisable)(GLenum);
typedef void (*PFN_glBlendEquation)(GLenum);
typedef void (*PFN_glBlendFunc)(GLenum, GLenum);
typedef void (*PFN_glScissor)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFN_glGetIntegerv)(GLenum, GLint *);
typedef void (*PFN_glGetFloatv)(GLenum, GLfloat *);
typedef void (*PFN_glDrawElements)(GLenum, GLsizei, GLenum, const void *);

static PFN_glGetString p_glGetString;
static PFN_glGenTextures p_glGenTextures;
static PFN_glBindTexture p_glBindTexture;
static PFN_glTexParameteri p_glTexParameteri;
static PFN_glTexImage2D p_glTexImage2D;
static PFN_glActiveTexture p_glActiveTexture;
static PFN_glGenBuffers p_glGenBuffers;
static PFN_glBindBuffer p_glBindBuffer;
static PFN_glBufferData p_glBufferData;
static PFN_glGenVertexArrays p_glGenVertexArrays;
static PFN_glBindVertexArray p_glBindVertexArray;
static PFN_glVertexAttribPointer p_glVertexAttribPointer;
static PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray;
static PFN_glCreateShader p_glCreateShader;
static PFN_glShaderSource p_glShaderSource;
static PFN_glCompileShader p_glCompileShader;
static PFN_glGetShaderiv p_glGetShaderiv;
static PFN_glGetShaderInfoLog p_glGetShaderInfoLog;
static PFN_glCreateProgram p_glCreateProgram;
static PFN_glAttachShader p_glAttachShader;
static PFN_glBindAttribLocation p_glBindAttribLocation;
static PFN_glLinkProgram p_glLinkProgram;
static PFN_glGetProgramiv p_glGetProgramiv;
static PFN_glGetProgramInfoLog p_glGetProgramInfoLog;
static PFN_glUseProgram p_glUseProgram;
static PFN_glGetUniformLocation p_glGetUniformLocation;
static PFN_glUniform1i p_glUniform1i;
static PFN_glUniformMatrix4fv p_glUniformMatrix4fv;
static PFN_glEnable p_glEnable;
static PFN_glDisable p_glDisable;
static PFN_glBlendEquation p_glBlendEquation;
static PFN_glBlendFunc p_glBlendFunc;
static PFN_glScissor p_glScissor;
static PFN_glGetIntegerv p_glGetIntegerv;
static PFN_glGetFloatv p_glGetFloatv;
static PFN_glDrawElements p_glDrawElements;

static void *sym(const char *name)
{
#ifdef _WIN32
    /* opengl32.dll exports only GL 1.1 directly; the rest come
       from wglGetProcAddress. */
    static HMODULE gl = NULL;
    if (!gl) gl = GetModuleHandleA("opengl32.dll");
    void *p = (void *)GetProcAddress(gl, name);
    if (!p) p = (void *)wglGetProcAddress(name);
    return p;
#else
    return dlsym(RTLD_DEFAULT, name);
#endif
}

static int gl_load(void)
{
    p_glGetString = (PFN_glGetString)sym("glGetString");
    p_glGenTextures = (PFN_glGenTextures)sym("glGenTextures");
    p_glBindTexture = (PFN_glBindTexture)sym("glBindTexture");
    p_glTexParameteri = (PFN_glTexParameteri)sym("glTexParameteri");
    p_glTexImage2D = (PFN_glTexImage2D)sym("glTexImage2D");
    p_glActiveTexture = (PFN_glActiveTexture)sym("glActiveTexture");
    p_glGenBuffers = (PFN_glGenBuffers)sym("glGenBuffers");
    p_glBindBuffer = (PFN_glBindBuffer)sym("glBindBuffer");
    p_glBufferData = (PFN_glBufferData)sym("glBufferData");
    p_glGenVertexArrays = (PFN_glGenVertexArrays)sym("glGenVertexArrays");
    p_glBindVertexArray = (PFN_glBindVertexArray)sym("glBindVertexArray");
    p_glVertexAttribPointer =
        (PFN_glVertexAttribPointer)sym("glVertexAttribPointer");
    p_glEnableVertexAttribArray =
        (PFN_glEnableVertexAttribArray)sym("glEnableVertexAttribArray");
    p_glCreateShader = (PFN_glCreateShader)sym("glCreateShader");
    p_glShaderSource = (PFN_glShaderSource)sym("glShaderSource");
    p_glCompileShader = (PFN_glCompileShader)sym("glCompileShader");
    p_glGetShaderiv = (PFN_glGetShaderiv)sym("glGetShaderiv");
    p_glGetShaderInfoLog = (PFN_glGetShaderInfoLog)sym("glGetShaderInfoLog");
    p_glCreateProgram = (PFN_glCreateProgram)sym("glCreateProgram");
    p_glAttachShader = (PFN_glAttachShader)sym("glAttachShader");
    p_glBindAttribLocation =
        (PFN_glBindAttribLocation)sym("glBindAttribLocation");
    p_glLinkProgram = (PFN_glLinkProgram)sym("glLinkProgram");
    p_glGetProgramiv = (PFN_glGetProgramiv)sym("glGetProgramiv");
    p_glGetProgramInfoLog = (PFN_glGetProgramInfoLog)sym("glGetProgramInfoLog");
    p_glUseProgram = (PFN_glUseProgram)sym("glUseProgram");
    p_glGetUniformLocation =
        (PFN_glGetUniformLocation)sym("glGetUniformLocation");
    p_glUniform1i = (PFN_glUniform1i)sym("glUniform1i");
    p_glUniformMatrix4fv = (PFN_glUniformMatrix4fv)sym("glUniformMatrix4fv");
    p_glEnable = (PFN_glEnable)sym("glEnable");
    p_glDisable = (PFN_glDisable)sym("glDisable");
    p_glBlendEquation = (PFN_glBlendEquation)sym("glBlendEquation");
    p_glBlendFunc = (PFN_glBlendFunc)sym("glBlendFunc");
    p_glScissor = (PFN_glScissor)sym("glScissor");
    p_glGetIntegerv = (PFN_glGetIntegerv)sym("glGetIntegerv");
    p_glGetFloatv = (PFN_glGetFloatv)sym("glGetFloatv");
    p_glDrawElements = (PFN_glDrawElements)sym("glDrawElements");

    int missing = 0;
    if (!p_glGetString || !p_glGenTextures || !p_glBindTexture ||
        !p_glTexParameteri || !p_glTexImage2D || !p_glGenBuffers ||
        !p_glBindBuffer || !p_glBufferData || !p_glVertexAttribPointer ||
        !p_glEnableVertexAttribArray || !p_glCreateShader ||
        !p_glShaderSource || !p_glCompileShader || !p_glCreateProgram ||
        !p_glAttachShader || !p_glLinkProgram || !p_glUseProgram ||
        !p_glGetUniformLocation || !p_glUniformMatrix4fv || !p_glEnable ||
        !p_glDisable || !p_glDrawElements || !p_glScissor)
        missing = 1;
    /* VAO + blend helpers are nice-to-have, tolerated if absent. */
    char buf[512];
    snprintf(buf, sizeof buf,
             "MRGS VR Chat: gl_load missing_core=%d vao=%p blendfn=%p\n",
             missing, (void *)p_glGenVertexArrays,
             (void *)p_glBlendFunc);
    XPLMDebugString(buf);
    return !missing;
}

static XPLMMenuID g_menu = NULL;
static int g_glOk = -1;          /* -1 unknown, 0 failed, 1 ready */
static int g_imguiReady = 0;
static GLuint g_fontTex = 0;
static GLuint g_prog = 0, g_vbo = 0, g_ebo = 0, g_vao = 0;
static GLint g_uTex = -1, g_uProj = -1;
static double g_lastTime = 0;
static int g_clicks = 0;
static int g_frameLog = 0;

static void log_win(const char *where)
{
    char buf[384];
    if (!g_win) {
        snprintf(buf, sizeof buf, "MRGS VR Chat [%s]: no window\n", where);
        XPLMDebugString(buf);
        return;
    }
    int L, T, R, B;
    XPLMGetWindowGeometry(g_win, &L, &T, &R, &B);
    snprintf(buf, sizeof buf,
             "MRGS VR Chat [%s]: visible=%d geom L%d T%d R%d B%d\n",
             where, XPLMGetWindowIsVisible(g_win), L, T, R, B);
    XPLMDebugString(buf);
}

/* ---------------- ImGui backend ---------------- */

static const char *VERT_SRC =
    "attribute vec2 Position;\n"
    "attribute vec2 UV;\n"
    "attribute vec4 Color;\n"
    "varying vec2 Frag_UV;\n"
    "varying vec4 Frag_Color;\n"
    "uniform mat4 ProjMtx;\n"
    "void main() {\n"
    "  Frag_UV = UV;\n"
    "  Frag_Color = Color;\n"
    "  gl_Position = ProjMtx * vec4(Position.xy, 0.0, 1.0);\n"
    "}\n";

static const char *FRAG_SRC =
    "varying vec2 Frag_UV;\n"
    "varying vec4 Frag_Color;\n"
    "uniform sampler2D Texture;\n"
    "void main() {\n"
    "  gl_FragColor = Frag_Color * texture2D(Texture, Frag_UV);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src, const char *tag)
{
    GLuint sh = p_glCreateShader(type);
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    if (p_glGetShaderiv) {
        GLint ok = 0;
        p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        char buf[640];
        if (!ok && p_glGetShaderInfoLog) {
            char logbuf[512];
            p_glGetShaderInfoLog(sh, sizeof logbuf, NULL, logbuf);
            snprintf(buf, sizeof buf,
                     "MRGS VR Chat: shader %s FAILED: %s\n", tag, logbuf);
        } else {
            snprintf(buf, sizeof buf,
                     "MRGS VR Chat: shader %s compiled ok=%d\n", tag,
                     (int)ok);
        }
        XPLMDebugString(buf);
        if (!ok) return 0;
    }
    return sh;
}

static int imgui_backend_init(void)
{
    if (!gl_load()) {
        XPLMDebugString("MRGS VR Chat: GL core entry points missing - "
                        "backend disabled\n");
        return 0;
    }
    if (p_glGetString) {
        char buf[512];
        const GLubyte *ver = p_glGetString(GL_VERSION);
        const GLubyte *ven = p_glGetString(GL_VENDOR);
        snprintf(buf, sizeof buf, "MRGS VR Chat: GL_VERSION=%s "
                 "GL_VENDOR=%s\n",
                 ver ? (const char *)ver : "?", ven ? (const char *)ven : "?");
        XPLMDebugString(buf);
    }

    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERT_SRC, "vert");
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAG_SRC, "frag");
    if (!vs || !fs) return 0;
    g_prog = p_glCreateProgram();
    p_glAttachShader(g_prog, vs);
    p_glAttachShader(g_prog, fs);
    if (p_glBindAttribLocation) {
        p_glBindAttribLocation(g_prog, 0, "Position");
        p_glBindAttribLocation(g_prog, 1, "UV");
        p_glBindAttribLocation(g_prog, 2, "Color");
    }
    p_glLinkProgram(g_prog);
    if (p_glGetProgramiv) {
        GLint ok = 0;
        p_glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
        char buf[256];
        snprintf(buf, sizeof buf, "MRGS VR Chat: program link ok=%d\n",
                 (int)ok);
        XPLMDebugString(buf);
        if (!ok) return 0;
    }
    g_uTex = p_glGetUniformLocation(g_prog, "Texture");
    g_uProj = p_glGetUniformLocation(g_prog, "ProjMtx");

    p_glGenBuffers(1, &g_vbo);
    p_glGenBuffers(1, &g_ebo);
    if (p_glGenVertexArrays && p_glBindVertexArray) {
        p_glGenVertexArrays(1, &g_vao);
    }

    /* font atlas */
    ImGuiIO &io = ImGui::GetIO();
    unsigned char *pixels = NULL;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &tw, &th);
    p_glGenTextures(1, &g_fontTex);
    p_glBindTexture(GL_TEXTURE_2D, g_fontTex);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, pixels);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)g_fontTex);
    char buf[256];
    snprintf(buf, sizeof buf,
             "MRGS VR Chat: font atlas %dx%d tex=%u prog=%u\n", tw, th,
             g_fontTex, g_prog);
    XPLMDebugString(buf);
    return 1;
}

static void imgui_render_draw_data(ImDrawData *dd, int winL, int winT,
                                   int winB)
{
    int scrL, scrT, scrR, scrB;
    XPLMGetScreenBoundsGlobal(&scrL, &scrT, &scrR, &scrB);
    float scrW = (float)(scrR - scrL), scrH = (float)(scrT - scrB);
    if (scrW < 1) scrW = 1920;
    if (scrH < 1) scrH = 1200;

    /* VR (restored in v1.2.1 - the DispatchUI renderer alone is
       blank in VR on Vulkan rigs, proven by v1.2.0): the window
       lives in panel coordinates, nowhere near the monitor bounds,
       so project in window-local space; the panel texture is
       exactly the window's size. */
    int inVR = (g_win && XPLMWindowIsInVR(g_win)) ? 1 : 0;
    /* GS4XP recipe (v1.3.2): a popped-out window also renders
       into its own surface, exactly like VR - the same
       window-local projection applies. Handled explicitly via
       XPLMWindowIsPoppedOut, as GS4XP's engine does. */
    int local = inVR || (g_win && XPLMWindowIsPoppedOut(g_win));
    if (local) {
        scrL = 0;
        scrB = 0;
        scrW = dd->DisplaySize.x;
        scrH = dd->DisplaySize.y;
        winL = 0;
        winT = (int)dd->DisplaySize.y;
        winB = 0;
    }

    /* local (x right, y down from window top) -> clip space */
    float a = 2.0f / scrW;
    float b = -2.0f / scrH;
    float c = 2.0f * (float)(winL - scrL) / scrW - 1.0f;
    float d = 2.0f * (float)(winT - scrB) / scrH - 1.0f;
    float proj[16] = { a, 0, 0, 0,  0, b, 0, 0,
                       0, 0, 1, 0,  c, d, 0, 1 };

    if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0);
    p_glUseProgram(g_prog);
    p_glUniform1i(g_uTex, 0);
    p_glUniformMatrix4fv(g_uProj, 1, GL_FALSE, proj);
    p_glEnable(GL_BLEND);
    if (p_glBlendEquation) p_glBlendEquation(GL_FUNC_ADD);
    if (p_glBlendFunc) p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_DEPTH_TEST);
    p_glEnable(GL_SCISSOR_TEST);

    if (g_vao && p_glBindVertexArray) p_glBindVertexArray(g_vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ebo);
    p_glEnableVertexAttribArray(0);
    p_glEnableVertexAttribArray(1);
    p_glEnableVertexAttribArray(2);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert),
                            (const void *)IM_OFFSETOF(ImDrawVert, pos));
    p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert),
                            (const void *)IM_OFFSETOF(ImDrawVert, uv));
    p_glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                            sizeof(ImDrawVert),
                            (const void *)IM_OFFSETOF(ImDrawVert, col));

    /* VR: scale the scissor rects into GL viewport pixels (the
       panel texture can be supersampled). No-op when they match. */
    float scX = 1.0f, scY = 1.0f;
    int vpW = 0, vpH = 0;
    if (local && p_glGetIntegerv) {
        GLint vp[4] = {0, 0, 0, 0};
        p_glGetIntegerv(GL_VIEWPORT, vp);
        vpW = vp[2];
        vpH = vp[3];
        if (vp[2] > 0 && dd->DisplaySize.x > 0)
            scX = (float)vp[2] / dd->DisplaySize.x;
        if (vp[3] > 0 && dd->DisplaySize.y > 0)
            scY = (float)vp[3] / dd->DisplaySize.y;
    }

    long totalIdx = 0, totalVtx = 0;
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList *cmds = dd->CmdLists[n];
        p_glBufferData(GL_ARRAY_BUFFER,
                       (GLsizeiptr)(cmds->VtxBuffer.Size *
                                    sizeof(ImDrawVert)),
                       cmds->VtxBuffer.Data, GL_STREAM_DRAW);
        p_glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                       (GLsizeiptr)(cmds->IdxBuffer.Size *
                                    sizeof(ImDrawIdx)),
                       cmds->IdxBuffer.Data, GL_STREAM_DRAW);
        totalVtx += cmds->VtxBuffer.Size;
        for (int i = 0; i < cmds->CmdBuffer.Size; i++) {
            const ImDrawCmd &cmd = cmds->CmdBuffer[i];
            if (cmd.UserCallback) continue;
            GLuint tex = (GLuint)(intptr_t)cmd.GetTexID();
            p_glBindTexture(GL_TEXTURE_2D, tex);
            int sx = winL + (int)(cmd.ClipRect.x * scX);
            int sy = winB +
                     (int)((dd->DisplaySize.y - cmd.ClipRect.w) * scY);
            p_glScissor(sx, sy,
                        (int)((cmd.ClipRect.z - cmd.ClipRect.x) * scX),
                        (int)((cmd.ClipRect.w - cmd.ClipRect.y) * scY));
            p_glDrawElements(GL_TRIANGLES, (GLsizei)cmd.ElemCount,
                             sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT
                                                    : GL_UNSIGNED_INT,
                             (const void *)(intptr_t)(cmd.IdxOffset *
                                                      sizeof(ImDrawIdx)));
            totalIdx += cmd.ElemCount;
        }
    }
    p_glDisable(GL_SCISSOR_TEST);
    if (g_frameLog < 3) {
        g_frameLog++;
        char buf[256];
        snprintf(buf, sizeof buf,
                 "MRGS VR Chat: frame %d submitted lists=%d vtx=%ld "
                 "idx=%ld screen=%.0fx%.0f vr=%d vp=%dx%d k=%.2fx%.2f\n",
                 g_frameLog, dd->CmdListsCount, totalVtx, totalIdx, scrW,
                 scrH, inVR, vpW, vpH, scX, scY);
        XPLMDebugString(buf);
    }
}

/* ---------------- state (Python engine contract) ---------------- */

#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include "json_mini.inc"
#include "font_roboto.inc"

static int g_light = 0; /* dark theme only in v1 */
static ImFont *g_fontTitle = NULL;   /* 22 px - headers */
static ImFont *g_fontDisplay = NULL; /* 40 px - unused headroom */

static char g_statePath[1024] = "";
static char g_actionsPath[1024] = "";
static char g_statePath2[1024] = "";
static char g_actionsPath2[1024] = "";
static int g_base = -1; /* 0 = XPLMGetSystemPath base, 1 = $HOME base */
static long g_seenMtime = -1;
static int g_pollLogged = 0, g_stateLogged = 0;
static JVal *g_state = NULL;
static double g_lastPoll = -1;
static long g_seenSize = -1;
static float g_uiW = 1008, g_uiH = 773; /* live window size, per frame */
static int g_themeLight = -1; /* applied theme: -1 none, 0 dark */

/* Background opacity, 0-100 (percent). 0 = panel invisible, chat
 * text floats over the cockpit; text/controls always stay opaque.
 * Persisted in mrgs_chat_ui_prefs.txt beside the state file. */
static float g_bgOpacity = 85.0f;
/* Chrome-free VR: in VR the X-Plane square window hides and the
   follow overlay becomes the display (cockpit + external). */
static float g_opacityF = 0.85f; /* g_bgOpacity / 100, set per frame */
static char g_prefsPath[1024] = "";
static char g_prefsPath2[1024] = "";

static void prefs_save(void)
{
    const char *path = (g_base == 1 && g_prefsPath2[0]) ? g_prefsPath2
                                                        : g_prefsPath;
    if (!path[0]) return;
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "opacity=%d\n", (int)(g_bgOpacity + 0.5f));
        fclose(f);
    }
}

static void prefs_load(void)
{
    for (int i = 0; i < 2; i++) {
        const char *path = i == 0 ? g_prefsPath : g_prefsPath2;
        if (!path[0]) continue;
        FILE *f = fopen(path, "r");
        if (f) {
            char buf[64] = "";
            if (fgets(buf, sizeof buf, f)) {
                const char *p = strstr(buf, "opacity=");
                if (p) {
                    int v = atoi(p + 8);
                    if (v < 0) v = 0;
                    if (v > 100) v = 100;
                    g_bgOpacity = (float)v;
                }
            }
            fclose(f);
            return;
        }
    }
}

static void send_action(const char *json)
{
    const char *path = (g_base == 1 && g_actionsPath2[0]) ? g_actionsPath2
                       : g_actionsPath;
    if (!path[0]) return;
    FILE *fh = fopen(path, "a");
    if (!fh) return;
    fputs(json, fh);
    fputc('\n', fh);
    fclose(fh);
}

static int vr_enabled()
{
    static XPLMDataRef ref = NULL;
    if (!ref) ref = XPLMFindDataRef("sim/graphics/VR/enabled");
    return ref ? (XPLMGetDatai(ref) != 0) : 0;
}

static void poll_state(void)
{
    double now = XPLMGetElapsedTime();
    if (g_lastPoll > 0 && now - g_lastPoll < 0.25) return;
    g_lastPoll = now;
    struct stat st;
    const char *path = NULL;
    int r0 = -1, e0 = 0, r1 = -1, e1 = 0;
    struct stat st0, st1;
    if (g_statePath[0]) { errno = 0; r0 = stat(g_statePath, &st0); e0 = errno; }
    if (r0 == 0) { path = g_statePath; st = st0; g_base = 0; }
    else if (g_statePath2[0]) {
        errno = 0; r1 = stat(g_statePath2, &st1); e1 = errno;
        if (r1 == 0) { path = g_statePath2; st = st1; g_base = 1; }
    }
    if (!g_pollLogged) {
        g_pollLogged = 1;
        char msg[2300];
        snprintf(msg, sizeof msg,
                 "MRGS VR Chat: poll primary stat=%d errno=%d path=%s | "
                 "alt stat=%d errno=%d path=%s\n",
                 r0, e0, g_statePath, r1, e1, g_statePath2);
        XPLMDebugString(msg);
    }
    if (!path) return;
    long mt = (long)st.st_mtime;
    if (g_state && (long)st.st_size == g_seenSize && mt == g_seenMtime) return;
    FILE *fh = fopen(path, "rb");
    if (!fh) return;
    char *buf = (char *)malloc((size_t)st.st_size + 1);
    if (!buf) { fclose(fh); return; }
    size_t rd = fread(buf, 1, (size_t)st.st_size, fh);
    fclose(fh);
    buf[rd] = 0;
    JVal *v = json_parse(buf);
    free(buf);
    if (v) {
        jfree(g_state);
        g_state = v;
        g_seenSize = (long)st.st_size;
        g_seenMtime = mt;
        if (!g_stateLogged) {
            g_stateLogged = 1;
            char msg[256];
            snprintf(msg, sizeof msg,
                     "MRGS VR Chat: state ok base=%d size=%ld\n",
                     g_base, g_seenSize);
            XPLMDebugString(msg);
        }
    }
}

/* ---------------- theme (shared AAA tokens) ---------------- */

static void apply_theme(int light)
{
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 22.0f;
    st.ChildRounding = 16.0f;
    st.FrameRounding = 12.0f;
    st.GrabRounding = 6.0f;
    st.WindowBorderSize = 0.0f;
    st.FrameBorderSize = 1.0f;
    st.FramePadding = ImVec2(10, 7);
    st.ItemSpacing = ImVec2(10, 9);
    st.WindowPadding = ImVec2(20, 16);
    if (!light) {
        st.Colors[ImGuiCol_WindowBg] = ImVec4(0.043f, 0.066f, 0.133f, 1.0f);
        st.Colors[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.106f, 0.196f, 1.0f);
        st.Colors[ImGuiCol_Text] = ImVec4(0.910f, 0.882f, 0.792f, 1.0f);
        st.Colors[ImGuiCol_TextDisabled] =
            ImVec4(0.545f, 0.580f, 0.667f, 1.0f);
        st.Colors[ImGuiCol_Button] = ImVec4(0.133f, 0.180f, 0.290f, 1.0f);
        st.Colors[ImGuiCol_ButtonHovered] =
            ImVec4(0.180f, 0.235f, 0.369f, 1.0f);
        st.Colors[ImGuiCol_ButtonActive] =
            ImVec4(0.220f, 0.282f, 0.435f, 1.0f);
        st.Colors[ImGuiCol_FrameBg] = ImVec4(0.102f, 0.145f, 0.243f, 1.0f);
        st.Colors[ImGuiCol_FrameBgHovered] =
            ImVec4(0.145f, 0.196f, 0.318f, 1.0f);
        st.Colors[ImGuiCol_FrameBgActive] =
            ImVec4(0.180f, 0.235f, 0.369f, 1.0f);
        st.Colors[ImGuiCol_CheckMark] = ImVec4(0.851f, 0.714f, 0.290f, 1.0f);
        st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.851f, 0.714f, 0.290f, 1.0f);
        st.Colors[ImGuiCol_SliderGrabActive] =
            ImVec4(0.937f, 0.812f, 0.427f, 1.0f);
        st.Colors[ImGuiCol_Border] = ImVec4(0.322f, 0.373f, 0.510f, 0.85f);
        st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.13f, 0.22f, 0.35f);
        st.Colors[ImGuiCol_ScrollbarGrab] =
            ImVec4(0.35f, 0.42f, 0.60f, 0.80f);
        st.Colors[ImGuiCol_ScrollbarGrabHovered] =
            ImVec4(0.42f, 0.50f, 0.68f, 0.85f);
        st.Colors[ImGuiCol_ScrollbarGrabActive] =
            ImVec4(0.50f, 0.58f, 0.76f, 0.90f);
        st.Colors[ImGuiCol_Separator] = ImVec4(0.220f, 0.263f, 0.376f, 1.0f);
        st.Colors[ImGuiCol_PlotHistogram] =
            ImVec4(0.851f, 0.714f, 0.290f, 1.0f);
    } else {
        st.Colors[ImGuiCol_WindowBg] = ImVec4(0.961f, 0.949f, 0.922f, 1.0f);
        st.Colors[ImGuiCol_ChildBg] = ImVec4(0.992f, 0.980f, 0.941f, 1.0f);
        st.Colors[ImGuiCol_Text] = ImVec4(0.086f, 0.137f, 0.247f, 1.0f);
        st.Colors[ImGuiCol_TextDisabled] =
            ImVec4(0.420f, 0.463f, 0.541f, 1.0f);
        st.Colors[ImGuiCol_Button] = ImVec4(0.929f, 0.906f, 0.847f, 1.0f);
        st.Colors[ImGuiCol_ButtonHovered] =
            ImVec4(0.894f, 0.863f, 0.780f, 1.0f);
        st.Colors[ImGuiCol_ButtonActive] =
            ImVec4(0.851f, 0.808f, 0.706f, 1.0f);
        st.Colors[ImGuiCol_FrameBg] = ImVec4(0.945f, 0.925f, 0.875f, 1.0f);
        st.Colors[ImGuiCol_FrameBgHovered] =
            ImVec4(0.914f, 0.886f, 0.816f, 1.0f);
        st.Colors[ImGuiCol_FrameBgActive] =
            ImVec4(0.871f, 0.831f, 0.733f, 1.0f);
        st.Colors[ImGuiCol_CheckMark] = ImVec4(0.561f, 0.435f, 0.078f, 1.0f);
        st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.561f, 0.435f, 0.078f, 1.0f);
        st.Colors[ImGuiCol_SliderGrabActive] =
            ImVec4(0.435f, 0.337f, 0.059f, 1.0f);
        st.Colors[ImGuiCol_Border] = ImVec4(0.698f, 0.631f, 0.463f, 0.90f);
        st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.85f, 0.80f, 0.64f, 0.35f);
        st.Colors[ImGuiCol_ScrollbarGrab] =
            ImVec4(0.72f, 0.64f, 0.44f, 0.80f);
        st.Colors[ImGuiCol_ScrollbarGrabHovered] =
            ImVec4(0.66f, 0.57f, 0.37f, 0.85f);
        st.Colors[ImGuiCol_ScrollbarGrabActive] =
            ImVec4(0.60f, 0.51f, 0.31f, 0.90f);
        st.Colors[ImGuiCol_Separator] = ImVec4(0.780f, 0.729f, 0.596f, 1.0f);
        st.Colors[ImGuiCol_PlotHistogram] =
            ImVec4(0.561f, 0.435f, 0.078f, 1.0f);
    }
    g_themeLight = light;
}

/* ---------------- UI helpers ---------------- */

static ImVec4 col_gold(void)
{
    return g_themeLight == 1 ? ImVec4(0.561f, 0.435f, 0.078f, 1.0f)
                             : ImVec4(0.851f, 0.714f, 0.290f, 1.0f);
}
static ImVec4 col_blue(void)
{
    return g_themeLight == 1 ? ImVec4(0.145f, 0.388f, 0.722f, 1.0f)
                             : ImVec4(0.478f, 0.686f, 0.965f, 1.0f);
}
static ImVec4 col_green(void)
{
    return ImVec4(0.298f, 0.788f, 0.463f, 1.0f);
}
static ImVec4 col_red(void) { return ImVec4(0.910f, 0.400f, 0.310f, 1.0f); }

static ImVec4 tone_color(const char *tone)
{
    if (tone && strcmp(tone, "gold") == 0) return col_gold();
    if (tone && strcmp(tone, "green") == 0) return col_green();
    if (tone && strcmp(tone, "dim") == 0)
        return ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    return ImGui::GetStyle().Colors[ImGuiCol_Text];
}

static ImU32 lerp_col(ImU32 a, ImU32 b, float f);
static void draw_grad_rounded(ImDrawList *dl, ImVec2 p, ImVec2 sz,
                              float rounding, ImU32 top, ImU32 bot);
static void card_shadow(const ImVec2 &p, const ImVec2 &sz);
static void card_bg(const ImVec2 &p, const ImVec2 &sz);
static void card_bevel(const ImVec2 &p, const ImVec2 &sz);
static void draw_zone_cursor(ImDrawList *dl, ImVec2 c, int kind);
static int g_cursorKind = 0;

/* Custom-drawn gradient button: gold primary. */
static int gold_button(const char *label, ImVec2 size)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    int hit = ImGui::InvisibleButton(label, size);
    int hov = ImGui::IsItemHovered();
    int act = ImGui::IsItemActive();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float yo = act ? 1.5f : 0.0f;
    if (!act)
        dl->AddRectFilled(ImVec2(p.x, p.y + 3),
                          ImVec2(p.x + size.x, p.y + size.y + 3),
                          IM_COL32(50, 38, 4, 70), 14.0f);
    ImU32 top = act ? IM_COL32(172, 130, 36, 255)
                    : (hov ? IM_COL32(236, 204, 112, 255)
                           : IM_COL32(222, 186, 88, 255));
    ImU32 bot = act ? IM_COL32(138, 102, 22, 255)
                    : (hov ? IM_COL32(192, 148, 48, 255)
                           : IM_COL32(166, 126, 32, 255));
    draw_grad_rounded(dl, ImVec2(p.x, p.y + yo), size, 14.0f, top, bot);
    dl->AddRect(ImVec2(p.x, p.y + yo),
                ImVec2(p.x + size.x, p.y + size.y + yo),
                IM_COL32(120, 92, 18, 255), 14.0f);
    dl->AddLine(ImVec2(p.x + 13, p.y + yo + 0.5f),
                ImVec2(p.x + size.x - 13, p.y + yo + 0.5f),
                IM_COL32(255, 255, 255, 170), 1.0f);
    dl->AddLine(ImVec2(p.x + 13, p.y + yo + size.y - 0.5f),
                ImVec2(p.x + size.x - 13, p.y + yo + size.y - 0.5f),
                IM_COL32(90, 62, 4, 130), 1.0f);
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 tp(p.x + (size.x - ts.x) * 0.5f,
              p.y + yo + (size.y - ts.y) * 0.5f);
    dl->AddText(ImVec2(tp.x, tp.y + 1), IM_COL32(96, 68, 6, 160), label);
    dl->AddText(tp, IM_COL32(255, 248, 226, 255), label);
    return hit;
}

/* Custom-drawn gradient button: quiet secondary. */
/* Multiply a colour's alpha by the background-opacity factor.
   v1.0.1: the opacity slider fades EVERYTHING except text - panel,
   borders, buttons, controls - so chrome colours route through
   here and text colours never do. */
static ImU32 fade_col(ImU32 c)
{
    unsigned int a = (c >> 24) & 0xFF;
    a = (unsigned int)((float)a * g_opacityF);
    return (c & 0x00FFFFFFu) | (a << 24);
}

static int soft_button(const char *label, ImVec2 size)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    int hit = ImGui::InvisibleButton(label, size);
    int hov = ImGui::IsItemHovered();
    int act = ImGui::IsItemActive();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float yo = act ? 1.5f : 0.0f;
    if (!act)
        dl->AddRectFilled(ImVec2(p.x, p.y + 2),
                          ImVec2(p.x + size.x, p.y + size.y + 2),
                          fade_col(IM_COL32(40, 32, 4, 50)), 12.0f);
    ImU32 top, bot, bord, txt;
    if (g_light) {
        top = hov ? IM_COL32(255, 253, 244, 255) : IM_COL32(253, 250, 240, 255);
        bot = hov ? IM_COL32(240, 230, 198, 255) : IM_COL32(234, 224, 192, 255);
        bord = IM_COL32(198, 178, 120, 255);
        txt = IM_COL32(72, 64, 40, 255);
    } else {
        top = hov ? IM_COL32(44, 60, 96, 255) : IM_COL32(36, 50, 82, 255);
        bot = hov ? IM_COL32(28, 40, 66, 255) : IM_COL32(21, 31, 54, 255);
        bord = IM_COL32(70, 84, 118, 255);
        txt = IM_COL32(226, 218, 192, 255);
    }
    top = fade_col(top);
    bot = fade_col(bot);
    bord = fade_col(bord); /* txt stays fully opaque - it is text */
    if (act) { ImU32 t2 = top; top = bot; bot = t2; }
    draw_grad_rounded(dl, ImVec2(p.x, p.y + yo), size, 12.0f, top, bot);
    dl->AddRect(ImVec2(p.x, p.y + yo),
                ImVec2(p.x + size.x, p.y + size.y + yo), bord, 12.0f);
    dl->AddLine(ImVec2(p.x + 11, p.y + yo + 0.5f),
                ImVec2(p.x + size.x - 11, p.y + yo + 0.5f),
                fade_col(g_light ? IM_COL32(255, 255, 255, 190)
                                 : IM_COL32(255, 255, 255, 56)),
                1.0f);
    dl->AddLine(ImVec2(p.x + 11, p.y + yo + size.y - 0.5f),
                ImVec2(p.x + size.x - 11, p.y + yo + size.y - 0.5f),
                fade_col(g_light ? IM_COL32(96, 74, 22, 80)
                                 : IM_COL32(0, 0, 0, 110)),
                1.0f);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f,
                       p.y + yo + (size.y - ts.y) * 0.5f),
                txt, label);
    return hit;
}

/* Dock tab: gold gradient pill when active, quiet text otherwise. */
/* iPadOS-style sidebar row: gold pill when active, gold dot marker */
static int nav_item(const char *label, int active)
{
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 size(w, 42);
    ImVec2 p = ImGui::GetCursorScreenPos();
    int hit = ImGui::InvisibleButton(label, size);
    int hov = ImGui::IsItemHovered();
    int act = ImGui::IsItemActive();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    /* pill is inset 4 px inside the item rect, like the mockup */
    ImVec2 pp(p.x + 4, p.y);
    ImVec2 ps(size.x - 8, size.y);
    if (active) {
        if (!act)
            dl->AddRectFilled(ImVec2(pp.x, pp.y + 2),
                              ImVec2(pp.x + ps.x, pp.y + ps.y + 2),
                              IM_COL32(50, 38, 4, 55), 12.0f);
        draw_grad_rounded(dl, pp, ps, 12.0f, IM_COL32(222, 186, 88, 255),
                          IM_COL32(166, 126, 32, 255));
        dl->AddRect(pp, ImVec2(pp.x + ps.x, pp.y + ps.y),
                    IM_COL32(120, 92, 18, 255), 12.0f);
        dl->AddLine(ImVec2(pp.x + 11, pp.y + 0.5f),
                    ImVec2(pp.x + ps.x - 11, pp.y + 0.5f),
                    IM_COL32(255, 255, 255, 150), 1.0f);
    } else if (hov) {
        dl->AddRectFilled(pp, ImVec2(pp.x + ps.x, pp.y + ps.y),
                          g_light ? IM_COL32(120, 95, 30, 26)
                                  : IM_COL32(255, 255, 255, 16),
                          12.0f);
    }
    dl->AddCircleFilled(ImVec2(pp.x + 15, pp.y + ps.y * 0.5f), 3.5f,
                        active ? IM_COL32(70, 48, 4, 255)
                               : IM_COL32(198, 158, 62, 255));
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(pp.x + 27, pp.y + (ps.y - ts.y) * 0.5f),
                active ? IM_COL32(74, 52, 6, 255)
                       : (g_light ? IM_COL32(84, 76, 56, 255)
                                  : IM_COL32(178, 180, 168, 255)),
                label);
    return hit;
}

static void label_value(const char *label, const char *value)
{
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(150);
    ImGui::Text("%s", value);
}

static ImU32 lerp_col(ImU32 a, ImU32 b, float f)
{
    int ar = (a >> 0) & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF,
        aa = (a >> 24) & 0xFF;
    int br = (b >> 0) & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF,
        ba = (b >> 24) & 0xFF;
    return IM_COL32(ar + (int)((br - ar) * f), ag + (int)((bg - ag) * f),
                    ab + (int)((bb - ab) * f), aa + (int)((ba - aa) * f));
}

/* Vertical gradient inside a rounded rect, drawn as horizontal bands
   whose ends follow the corner arcs. */
static void draw_grad_rounded(ImDrawList *dl, ImVec2 p, ImVec2 sz,
                              float rounding, ImU32 top, ImU32 bot)
{
    const int bands = 18;
    for (int i = 0; i < bands; i++) {
        float y0 = p.y + sz.y * (float)i / bands;
        float y1 = p.y + sz.y * (float)(i + 1) / bands;
        float ym = (y0 + y1) * 0.5f;
        float inset = 0.0f, dy = -1.0f;
        if (ym < p.y + rounding)
            dy = ym - p.y;
        else if (ym > p.y + sz.y - rounding)
            dy = (p.y + sz.y) - ym;
        if (dy >= 0.0f) {
            float d = rounding - dy;
            inset = rounding - sqrtf(rounding * rounding - d * d);
        }
        ImU32 c = lerp_col(top, bot, (float)i / (float)(bands - 1));
        dl->AddRectFilled(ImVec2(p.x + inset, y0),
                          ImVec2(p.x + sz.x - inset, y1 + 0.6f), c);
    }
}

/* Layered drop shadow (contact + mid + wide) for cards. */
static void card_shadow(const ImVec2 &p, const ImVec2 &sz)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (int i = 4; i >= 1; i--) {
        float grow = (float)i * 1.5f;
        ImU32 a = g_light ? (ImU32)(26 - i * 4) : (ImU32)(40 - i * 6);
        dl->AddRectFilled(ImVec2(p.x - grow, p.y - grow + 3),
                          ImVec2(p.x + sz.x + grow, p.y + sz.y + grow + 3),
                          IM_COL32(0, 0, 0, a), 18.0f);
    }
}

/* Gradient card background + border at the cursor; content goes in a
   transparent child on top. */
static void card_bg(const ImVec2 &p, const ImVec2 &sz)
{
    card_shadow(p, sz);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 top = g_light ? IM_COL32(255, 254, 249, 255)
                        : IM_COL32(27, 39, 66, 255);
    ImU32 bot = g_light ? IM_COL32(243, 238, 226, 255)
                        : IM_COL32(13, 20, 38, 255);
    draw_grad_rounded(dl, p, sz, 18.0f, top, bot);
    ImU32 bord = g_light ? IM_COL32(203, 190, 152, 255)
                         : IM_COL32(70, 84, 118, 255);
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), bord, 18.0f);
}

static void card_bevel(const ImVec2 &p, const ImVec2 &sz)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 hi = g_light ? IM_COL32(255, 255, 255, 210)
                       : IM_COL32(255, 255, 255, 64);
    ImU32 hi2 = g_light ? IM_COL32(255, 255, 255, 80)
                        : IM_COL32(255, 255, 255, 24);
    ImU32 lo = g_light ? IM_COL32(96, 74, 22, 110) : IM_COL32(0, 0, 0, 150);
    /* rolled top edge: a bright line plus a soft echo under it */
    dl->AddLine(ImVec2(p.x + 17, p.y + 0.5f),
                ImVec2(p.x + sz.x - 17, p.y + 0.5f), hi, 1.0f);
    dl->AddLine(ImVec2(p.x + 18, p.y + 1.5f),
                ImVec2(p.x + sz.x - 18, p.y + 1.5f), hi2, 1.0f);
    dl->AddLine(ImVec2(p.x + 17, p.y + sz.y - 0.5f),
                ImVec2(p.x + sz.x - 17, p.y + sz.y - 0.5f), lo, 1.0f);
    dl->AddLine(ImVec2(p.x + 18, p.y + sz.y - 1.5f),
                ImVec2(p.x + sz.x - 18, p.y + sz.y - 1.5f),
                g_light ? IM_COL32(96, 74, 22, 45) : IM_COL32(0, 0, 0, 60),
                1.0f);
}

/* Draw-list card text: one coordinate system for card + text, so
   nothing can drift out of alignment (the child-window version did). */
static void dl_text(ImDrawList *dl, float x, float y, ImU32 col,
                    const char *txt)
{
    dl->AddText(ImVec2(x, y), col, txt);
}
static void dl_text_f(ImDrawList *dl, ImFont *font, float size, float x,
                      float y, ImU32 col, const char *txt)
{
    if (font)
        dl->AddText(font, size, ImVec2(x, y), col, txt);
    else
        dl->AddText(ImVec2(x, y), col, txt);
}
/* draw text clipped to maxw, ellipsized with "..." when too long -
   card text must never spill past the card edge */
static void dl_text_fit(ImDrawList *dl, float x, float y, float maxw,
                        ImU32 col, const char *txt)
{
    if (!txt) txt = "";
    ImFont *f = ImGui::GetFont();
    float fs = ImGui::GetFontSize();
    if (f && f->CalcTextSizeA(fs, 1e9f, 0, txt).x > maxw) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", txt);
        size_t n = strlen(tmp);
        while (n > 4) {
            tmp[--n] = 0;
            char cand[260];
            snprintf(cand, sizeof cand, "%s...", tmp);
            if (f->CalcTextSizeA(fs, 1e9f, 0, cand).x <= maxw) {
                dl->AddText(ImVec2(x, y), col, cand);
                return;
            }
        }
    }
    dl->AddText(ImVec2(x, y), col, txt);
}
static ImU32 u32_of(const ImVec4 &v)
{
    return ImGui::ColorConvertFloat4ToU32(v);
}

static void stat_tile(const char *label, const char *value, float w)
{
    ImVec2 cp = ImGui::GetCursorScreenPos();
    card_bg(cp, ImVec2(w, 64));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl_text(dl, cp.x + 13, cp.y + 9,
            ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    dl_text_f(dl, g_fontTitle, 22.0f, cp.x + 13, cp.y + 30,
              ImGui::GetColorU32(ImGuiCol_Text), value);
    card_bevel(cp, ImVec2(w, 64));
    ImGui::Dummy(ImVec2(w, 64));
}

/* ---------------- pages ---------------- */

/* "4806" -> "4,806" for hero detail lines */
static void fmt_comma(char *out, size_t n, long v)
{
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%ld", v);
    size_t len = strlen(tmp), o = 0;
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        if (i > 0 && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = tmp[i];
    }
    out[o] = 0;
}

static void upper_copy(char *out, size_t n, const char *in)
{
    size_t i = 0;
    for (; in[i] && i + 1 < n; i++)
        out[i] = (in[i] >= 'a' && in[i] <= 'z') ? (char)(in[i] - 32)
                                                : in[i];
    out[i] = 0;
}

/* ---------------- the chat window ---------------- */

/* Session-ID form state (v1.0.1): the ID is entered right in the
   window - click the field and type. The buffer never holds the
   saved ID (the engine keeps it; the state file only reports
   has_session), it only carries what the user just typed. */
static char g_idBuf[128] = "";
static int g_showSetup = 0;
static int g_kbFocus = 0;

static void send_session_id(const char *id)
{
    char out[600];
    int o = 0;
    o += snprintf(out + o, sizeof out - (size_t)o,
                  "{\"action\":\"set_session\",\"id\":\"");
    for (const char *p = id; *p && o < (int)sizeof out - 8; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 32) {
            o += snprintf(out + o, sizeof out - (size_t)o,
                          "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    snprintf(out + o, sizeof out - (size_t)o, "\"}");
    send_action(out);
}

static void session_form(const JVal *s, int hasSession)
{
    if (hasSession) {
        ImGui::TextColored(col_green(),
                           "Session ID saved - chat uses it now.");
        ImGui::TextWrapped("Paste a different ID below to replace "
                           "it, or remove it to disconnect.");
        ImGui::Spacing();
    } else {
        ImGui::TextWrapped("Enter your Social Stream Ninja session "
                           "ID to connect your chat:");
        ImGui::Spacing();
    }
    ImGui::PushItemWidth(-1);
    int enter = ImGui::InputTextWithHint(
        "##ssid", "Paste session ID", g_idBuf, sizeof g_idBuf,
        ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemActive()) g_kbFocus = 1;
    ImGui::PopItemWidth();
    ImGui::Spacing();
    int save = enter != 0;
    if (soft_button(hasSession ? "SAVE NEW ID" : "CONNECT",
                    ImVec2(170, 38)))
        save = 1;
    if (save && g_idBuf[0]) {
        send_session_id(g_idBuf);
        g_idBuf[0] = 0;
        g_showSetup = 0;
    }
    if (hasSession) {
        ImGui::SameLine();
        if (soft_button("REMOVE ID", ImVec2(140, 38))) {
            send_session_id("");
            g_showSetup = 0;
        }
        ImGui::SameLine();
        if (soft_button("DONE", ImVec2(100, 38))) g_showSetup = 0;
    }
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextDisabled("Find the ID in the Social Stream Ninja app");
    ImGui::TextDisabled("under Stream Deck Setup, or in the extension");
    ImGui::TextDisabled("under Global settings - Session Options.");
    ImGui::TextDisabled("You can also set it from a browser:");
    ImGui::TextColored(col_gold(), "%s",
                       jgs(s, "setup_url", "http://127.0.0.1:17890/"));
    ImGui::TextDisabled("Stored on this computer only, never shown back.");
}

/* VR safe area: X-Plane overlays its own window bar on the top
   strip of a VR window texture and the panel edges can crop, so
   in VR the whole UI is inset into the clean middle of the
   texture. Zero in 2D (pixel-perfect there). */
static float g_uiOX = 0, g_uiOY = 0, g_uiOB = 0;


static void build_chat_window(float winW, float winH)
{
    poll_state();
    const JVal *s = g_state;
    apply_theme(0);
    g_light = 0;
    g_kbFocus = 0;
    g_opacityF = g_bgOpacity / 100.0f;
    /* v1.0.1: opacity fades EVERYTHING except text. apply_theme()
       just reset the style, so scaling the fresh colours once per
       frame is exact - every non-text style colour (frames,
       borders, slider, scrollbar, separators) takes the factor.
       Text and TextDisabled keep their full alpha. */
    {
        ImGuiStyle &st = ImGui::GetStyle();
        for (int i = 0; i < ImGuiCol_COUNT; i++) {
            if (i == ImGuiCol_Text || i == ImGuiCol_TextDisabled)
                continue;
            st.Colors[i].w *= g_opacityF;
        }
    }
    g_uiW = winW;
    g_uiH = winH;

    ImGui::SetNextWindowPos(ImVec2(g_uiOX, g_uiOY));
    ImGui::SetNextWindowSize(ImVec2(winW - 2 * g_uiOX,
                                    winH - g_uiOY - g_uiOB));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoScrollbar;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("MRGS VR Chat Bridge", NULL, flags);
    ImGui::PopStyleColor();

    /* The ONLY thing the opacity slider fades: this background.
       Text, names and controls are drawn fully opaque on top. */
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        int alpha = (int)(g_bgOpacity * 2.55f + 0.5f);
        if (alpha < 0) alpha = 0;
        if (alpha > 255) alpha = 255;
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        draw_grad_rounded(dl, wp, ws, 18.0f,
                          IM_COL32(19, 28, 50, alpha),
                          IM_COL32(8, 13, 26, alpha));
        dl->AddRect(ImVec2(wp.x + 0.5f, wp.y + 0.5f),
                    ImVec2(wp.x + ws.x - 0.5f, wp.y + ws.y - 0.5f),
                    IM_COL32(120, 130, 160, alpha), 18.0f, 0, 1.0f);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 16));
    ImGui::BeginChild("main", ImVec2(0, 0), false,
                      ImGuiWindowFlags_NoScrollbar);


    /* ---- header: brand block ---- */
    {
        ImDrawList *hdl = ImGui::GetWindowDrawList();
        ImVec2 cp = ImGui::GetCursorScreenPos();
        int ba = (int)(255.0f * g_opacityF);
        if (ba < 0) ba = 0;
        if (ba > 255) ba = 255;
        ImU32 bgold = IM_COL32(232, 194, 104, ba);
        float bx = cp.x, by = cp.y + 1.0f;
        hdl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + 32, by + 25),
                           bgold, 7.0f);
        hdl->AddTriangleFilled(ImVec2(bx + 7, by + 23),
                               ImVec2(bx + 16, by + 23),
                               ImVec2(bx + 8, by + 32), bgold);
        for (int i = 0; i < 3; i++)
            hdl->AddCircleFilled(
                ImVec2(bx + 8.5f + i * 7.5f, by + 12.5f), 2.1f,
                IM_COL32(10, 16, 30, ba));
        ImGui::Dummy(ImVec2(40, 30));
        ImGui::SameLine();
        ImGui::BeginGroup();
    }
    if (g_fontTitle) ImGui::PushFont(g_fontTitle);
    ImGui::TextColored(ImVec4(0.949f, 0.925f, 0.851f, 1.0f), "MrGameSimulation");
    if (g_fontTitle) ImGui::PopFont();
    ImGui::TextColored(col_gold(), "VR CHAT BRIDGE");
    ImGui::EndGroup();
    int connected = s && jgb(s, "connected", 0);
    int hasSession = s && jgb(s, "has_session", 0);
    if (!s)
        ImGui::TextDisabled("STARTING...");
    else if (connected)
        ImGui::TextColored(col_green(), "LIVE - SOCIAL STREAM NINJA");
    else if (hasSession)
        ImGui::TextColored(col_gold(), "CONNECTING...");
    else
        ImGui::TextColored(col_red(), "NO SESSION ID - SETUP NEEDED");
    ImGui::TextDisabled("by Mandraki Bey - twitch.tv/mrgamesimulation");
    ImGui::Separator();

    /* ---- body ---- */
    float footerH = 52.0f;
    if (!s) {
        ImGui::TextWrapped("Waiting for the chat engine...");
        ImGui::Spacing();
        ImGui::TextDisabled("If this stays, check that XPPython3 is "
                            "installed and PI_MRGSChatBridge.py is in "
                            "Resources/plugins/PythonPlugins.");
    } else if (!hasSession || g_showSetup) {
        session_form(s, hasSession);
    } else {
        const JVal *msgs = jg(s, "messages");
        int n = msgs ? jlen(msgs) : 0;
        ImGui::BeginChild("chatfeed",
                          ImVec2(0, ImGui::GetContentRegionAvail().y -
                                        footerH),
                          false);
        if (!n) {
            ImGui::TextDisabled(connected
                ? "No messages yet - chat lands here as it arrives."
                : "Connecting to Social Stream Ninja...");
        }
        for (int i = 0; i < n; i++) {
            const JVal *m = jat(msgs, i);
            ImGui::TextColored(col_gold(), "%s", jgs(m, "name", ""));
            const char *src = jgs(m, "source", "");
            if (src[0]) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", src);
            }
            ImGui::TextWrapped("%s", jgs(m, "text", ""));
            ImGui::Dummy(ImVec2(0, 5));
        }
        static int s_chatCount = -1;
        if (n != s_chatCount) {
            s_chatCount = n;
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    /* ---- footer: clear + background opacity + hide ---- */
    /* Responsive: anchor to the EFFECTIVE window height (after
       the VR safe-area insets), never the raw texture height,
       or the footer slides off the bottom in VR. */
    float effH = winH - g_uiOY - g_uiOB;
    ImGui::SetCursorPosY(effH - 32.0f - footerH + 14.0f);
    ImGui::Separator();
    if (soft_button("CLEAR", ImVec2(96, 36)))
        send_action("{\"action\":\"clear\"}");
    ImGui::SameLine();
    ImGui::TextDisabled("BG");
    ImGui::SameLine();
    ImGui::PushItemWidth(150);
    float pct = g_bgOpacity;
    if (ImGui::SliderFloat("##bgopacity", &pct, 0.0f, 100.0f,
                           "%.0f%%")) {
        g_bgOpacity = pct;
        prefs_save();
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (soft_button("HIDE", ImVec2(84, 36)))
        XPLMSetWindowIsVisible(g_win, 0);
    if (hasSession) {
        ImGui::SameLine();
        if (soft_button("SETUP", ImVec2(96, 36)))
            g_showSetup = g_showSetup ? 0 : 1;
    }
    if (s) {
        ImGui::SameLine();
        ImGui::TextDisabled("v%s", jgs(s, "version", "1.0.0"));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    /* Keyboard focus follows the session-ID field: while it is
       being edited the XPLM window takes keystrokes (handle_key
       feeds them to ImGui); otherwise keys belong to the sim. */
    {
        static int s_kbApplied = -1;
        if (g_kbFocus != s_kbApplied) {
            s_kbApplied = g_kbFocus;
            XPLMTakeKeyboardFocus(g_kbFocus ? g_win : 0);
        }
    }
    ImGui::End();
}

static ImGuiContext *g_ctxs[3] = { NULL, NULL, NULL };
static int g_ctxTheme[3] = { -1, -1, -1 };
static ImFontAtlas *g_atlas = NULL;

static void do_frame(XPLMWindowID win, void (*build)(float, float),
                     int slot)
{
    int L, T, R, B;
    XPLMGetWindowGeometry(win, &L, &T, &R, &B);
    float W = (float)(R - L), H = (float)(T - B);
    if (W < 50 || H < 50) return;

    if (!g_ctxs[slot]) {
        if (!g_atlas) {
            ImGui::CreateContext();
            ImGuiIO &io = ImGui::GetIO();
            io.IniFilename = NULL;
            /* Real TTF at full size with oversampling - the stock
               pixel font is what looked pixelated. */
            ImFontConfig fc;
            fc.OversampleH = 3;
            fc.OversampleV = 2;
            fc.PixelSnapH = false;
            fc.FontDataOwnedByAtlas = false;
            io.Fonts->AddFontFromMemoryTTF((void *)kFontRoboto,
                                           (int)kFontRobotoLen, 17.0f,
                                           &fc);
            g_fontTitle = io.Fonts->AddFontFromMemoryTTF(
                (void *)kFontRoboto, (int)kFontRobotoLen, 22.0f, &fc);
            g_fontDisplay = io.Fonts->AddFontFromMemoryTTF(
                (void *)kFontRoboto, (int)kFontRobotoLen, 40.0f, &fc);
            io.FontGlobalScale = 1.0f;
            g_atlas = io.Fonts;
            apply_theme(g_light);
            g_glOk = imgui_backend_init();
            g_imguiReady = g_glOk;
        } else {
            ImGui::CreateContext(g_atlas);
            ImGuiIO &io = ImGui::GetIO();
            io.IniFilename = NULL;
            io.FontGlobalScale = 1.0f;
            apply_theme(g_light);
        }
        g_ctxTheme[slot] = g_light;
        g_ctxs[slot] = ImGui::GetCurrentContext();
        if (!g_imguiReady) {
            float red[3] = { 1.0f, 0.3f, 0.3f };
            XPLMDrawString(red, L + 16, T - 30,
                           (char *)"MRGS VR Chat: GL backend unavailable "
                                   "(see Log.txt)",
                           NULL, xplmFont_Proportional);
            return;
        }
    }
    if (!g_imguiReady) return;
    ImGui::SetCurrentContext(g_ctxs[slot]);
    if (g_ctxTheme[slot] != g_light) {
        apply_theme(g_light);
        g_ctxTheme[slot] = g_light;
    }

    ImGuiIO &io = ImGui::GetIO();
    double now = XPLMGetElapsedTime();
    io.DeltaTime = (g_lastTime > 0 && now > g_lastTime)
                       ? (float)(now - g_lastTime)
                       : 1.0f / 60.0f;
    g_lastTime = now;
    io.DisplaySize = ImVec2(W, H);

    int mx, my;
    XPLMGetMouseLocation(&mx, &my);
    io.AddMousePosEvent((float)(mx - L), (float)(T - my));

    ImGui::NewFrame();
    build(W, H);
    ImGui::Render();
    imgui_render_draw_data(ImGui::GetDrawData(), L, T, B);
}

/* ---------------- window callbacks ---------------- */

static void draw_window(XPLMWindowID win, void *ref)
{
    (void)win; (void)ref;
    do_frame(g_win, build_chat_window, 0);
}

static int g_dragging = 0;
static int g_dragDX = 0, g_dragDY = 0;
static int g_resizing = 0; /* bit 1: right edge, bit 2: bottom edge */

static int handle_click(XPLMWindowID win, int x, int y,
                        XPLMMouseStatus status, void *ref)
{
    (void)win; (void)ref;
    if (!g_imguiReady) return 1;
    int L, T, R, B;
    XPLMGetWindowGeometry(g_win, &L, &T, &R, &B);
    int W = R - L, H = T - B;
    /* Self-decorated window: resize from the right/bottom edges,
       move by the top header strip or the outer frame. The footer
       (buttons + slider) is never a drag region. */
    if (status == xplm_MouseDown && !g_dragging && !g_resizing) {
        int fromTop = T - y;
        int fromLeft = x - L;
        int rz = (fromLeft > W - 14 ? 1 : 0) | (fromTop > H - 14 ? 2 : 0);
        if (rz) {
            g_resizing = rz;
            return 1;
        }
        if (fromTop < 58 || fromLeft < 10 || fromLeft > W - 10 ||
            fromTop < 10) {
            g_dragging = 1;
            g_dragDX = x - L;
            g_dragDY = T - y;
            return 1;
        }
    }
    if (status == xplm_MouseDrag && g_resizing) {
        int nR = R, nB = B;
        if (g_resizing & 1) {
            int nW = x - L;
            if (nW < 420) nW = 420;
            if (nW > 1100) nW = 1100;
            nR = L + nW;
        }
        if (g_resizing & 2) {
            int nH = T - y;
            if (nH < 480) nH = 480;
            if (nH > 1100) nH = 1100;
            nB = T - nH;
        }
        XPLMSetWindowGeometry(g_win, L, T, nR, nB);
        return 1;
    }
    if (status == xplm_MouseDrag && g_dragging) {
        int nL = x - g_dragDX;
        int nT = y + g_dragDY;
        XPLMSetWindowGeometry(g_win, nL, nT, nL + W, nT - H);
        return 1;
    }
    if (status == xplm_MouseUp && (g_dragging || g_resizing)) {
        g_dragging = 0;
        g_resizing = 0;
        return 1;
    }
    if (!g_ctxs[0]) return 1;
    ImGui::SetCurrentContext(g_ctxs[0]);
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent((float)(x - L), (float)(T - y));
    if (status == xplm_MouseDown) io.AddMouseButtonEvent(0, true);
    if (status == xplm_MouseUp) io.AddMouseButtonEvent(0, false);
    return 1;
}

static int handle_wheel(XPLMWindowID win, int x, int y, int wheel,
                        int clicks, void *ref)
{
    (void)win; (void)x; (void)y; (void)ref;
    if (!g_imguiReady) return 1;
    if (!g_ctxs[0]) return 1;
    ImGui::SetCurrentContext(g_ctxs[0]);
    if (wheel == 0)
        ImGui::GetIO().AddMouseWheelEvent(0.0f, (float)clicks);
    return 1;
}

static void handle_key(XPLMWindowID win, char key, XPLMKeyFlags flags,
                       char virtualKey, void *ref, int losingFocus)
{
    (void)win; (void)ref; (void)losingFocus;
    if (!g_ctxs[0]) return;
    ImGui::SetCurrentContext(g_ctxs[0]);
    ImGuiIO &io = ImGui::GetIO();
    int down = (flags & xplm_DownFlag) != 0;
    io.AddKeyEvent(ImGuiMod_Shift, (flags & xplm_ShiftFlag) != 0);
    io.AddKeyEvent(ImGuiMod_Ctrl, (flags & xplm_ControlFlag) != 0);
    ImGuiKey k = ImGuiKey_None;
    switch ((unsigned char)virtualKey) {
        case XPLM_VK_BACK:   k = ImGuiKey_Backspace; break;
        case XPLM_VK_DELETE: k = ImGuiKey_Delete; break;
        case XPLM_VK_LEFT:   k = ImGuiKey_LeftArrow; break;
        case XPLM_VK_RIGHT:  k = ImGuiKey_RightArrow; break;
        case XPLM_VK_HOME:   k = ImGuiKey_Home; break;
        case XPLM_VK_END:    k = ImGuiKey_End; break;
        case XPLM_VK_RETURN: k = ImGuiKey_Enter; break;
        case XPLM_VK_ESCAPE: k = ImGuiKey_Escape; break;
    }
    if (k != ImGuiKey_None) io.AddKeyEvent(k, down != 0);
    if (down && key >= 32 && key < 127)
        io.AddInputCharacter((unsigned int)key);
}

static XPLMCursorStatus handle_cursor(XPLMWindowID win, int x, int y,
                                      void *ref)
{
    (void)win; (void)x; (void)y; (void)ref;
    return xplm_CursorDefault;
}

/* ---------------- lifecycle ---------------- */

static void create_window(void)
{
    if (g_win) return;
    int sl = 0, st = 0, sr = 1920, sb = 0;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    int w = 1008, h = 773;
    int cx = sl + ((sr - sl) - w) / 2;
    int cy = sb + ((st - sb) - h) / 2;
    XPLMCreateWindow_t p;
    memset(&p, 0, sizeof p);
    p.structSize = sizeof p;
    p.left = cx;
    p.top = cy + h;
    p.right = cx + w;
    p.bottom = cy;
    p.visible = 1;
    p.drawWindowFunc = draw_window;
    p.handleMouseClickFunc = handle_click;
    p.handleRightClickFunc = NULL;
    p.handleMouseWheelFunc = handle_wheel;
    p.handleKeyFunc = handle_key;
    p.handleCursorFunc = handle_cursor;
    p.refcon = NULL;
    p.decorateAsFloatingWindow = xplm_WindowDecorationSelfDecorated;
    p.layer = xplm_WindowLayerFloatingWindows;
    g_win = XPLMCreateWindowEx(&p);
    if (g_win) XPLMSetWindowResizingLimits(g_win, 640, 480, 1400, 1100);
    log_win("create");
}

/* 2D geometry saved when VR starts, restored when it ends: a VR
   window's panel coordinates (x ~ 200000) are meaningless on the
   desktop, so without this the window is lost off-screen in 2D. */
static int g_saveGeom[4] = { 0, 0, 0, 0 };
static int g_haveSaveGeom = 0;

static void window_recenter_2d(void)
{
    int sl, st, sr, sb;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    int w = 1008, h = 773;
    int l = sl + ((sr - sl) - w) / 2;
    int t = st - ((st - sb) - h) / 2;
    XPLMSetWindowGeometry(g_win, l, t, l + w, t - h);
}

static void menu_handler(void *menuRef, void *itemRef)
{
    (void)menuRef;
    if (itemRef == (void *)1) {
        if (!g_win) create_window();
        if (g_win) {
            int vis = XPLMGetWindowIsVisible(g_win);
            XPLMSetWindowIsVisible(g_win, vis ? 0 : 1);
            if (!vis) {
                XPLMBringWindowToFront(g_win);
                if (XPLMWindowIsInVR(g_win))
                    XPLMSetWindowGeometryVR(g_win, 1008, 773);
                if (!XPLMWindowIsInVR(g_win)) {
                    int l, t, r, b, sl, st, sr, sb;
                    XPLMGetWindowGeometry(g_win, &l, &t, &r, &b);
                    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
                    if (r <= sl || l >= sr || t <= sb || b >= st) {
                        if (g_haveSaveGeom)
                            XPLMSetWindowGeometry(
                                g_win, g_saveGeom[0], g_saveGeom[1],
                                g_saveGeom[2], g_saveGeom[3]);
                        else
                            window_recenter_2d();
                    }
                }
            }
            log_win("menu-toggle");
        }
    }
}


PLUGIN_API int XPluginStart(char *name, char *sig, char *desc)
{
    strcpy(name, "MrGameSimulation VR Chat Bridge");
    strcpy(sig, "mrgs.vrchatbridge.ui");
    strcpy(desc, "MrGameSimulation VR Chat Bridge - Social Stream "
                 "Ninja chat window for X-Plane (desktop and VR). "
                 "Logic stays in the MRGS Python engine.");
    XPLMDebugString("MRGS VR Chat: window v1.4.1 start\n");
    char sys[1024];
    XPLMGetSystemPath(sys);
    size_t n = strlen(sys);
    const char *sep = (n && sys[n - 1] == '/') ? "" : "/";
    snprintf(g_statePath, sizeof g_statePath, "%s%s%s", sys, sep,
             "Resources/plugins/PythonPlugins/mrgs_chat_state.json");
    snprintf(g_actionsPath, sizeof g_actionsPath, "%s%s%s", sys, sep,
             "Resources/plugins/PythonPlugins/mrgs_chat_actions.jsonl");
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(g_statePath2, sizeof g_statePath2, "%s%s", home,
                 "/X-Plane 12/Resources/plugins/PythonPlugins/"
                 "mrgs_chat_state.json");
        snprintf(g_actionsPath2, sizeof g_actionsPath2, "%s%s", home,
                 "/X-Plane 12/Resources/plugins/PythonPlugins/"
                 "mrgs_chat_actions.jsonl");
        snprintf(g_prefsPath2, sizeof g_prefsPath2, "%s%s", home,
                 "/X-Plane 12/Resources/plugins/PythonPlugins/"
                 "mrgs_chat_ui_prefs.txt");
    }
    snprintf(g_prefsPath, sizeof g_prefsPath, "%s%s%s", sys, sep,
             "Resources/plugins/PythonPlugins/mrgs_chat_ui_prefs.txt");
    prefs_load();
    int menuIdx = XPLMAppendMenuItem(XPLMFindPluginsMenu(),
                                     "MRGS VR Chat Bridge", NULL, 0);
    g_menu = XPLMCreateMenu("MRGS VR Chat Bridge",
                            XPLMFindPluginsMenu(), menuIdx, menu_handler,
                            NULL);
    XPLMAppendMenuItem(g_menu, "Show / Hide", (void *)1, 0);
    create_window();
    return 1;
}

PLUGIN_API int XPluginEnable(void)
{
    if (!g_win) create_window();
    log_win("enable");
    return 1;
}

PLUGIN_API void XPluginDisable(void)
{
    if (g_win) {
        XPLMDestroyWindow(g_win);
        g_win = NULL;
    }
    if (g_menu) {
        XPLMDestroyMenu(g_menu);
        g_menu = NULL;
    }
}

PLUGIN_API void XPluginStop(void)
{
}




PLUGIN_API void XPluginReceiveMessage(XPLMPluginID from, int msg,
                                      void *param)
{
    (void)from; (void)param;
    if (!g_win) return;
    if (msg == XPLM_MSG_ENTERED_VR) {
        g_frameLog = 0; /* log the first VR frames too */
        XPLMSetWindowPositioningMode(g_win, xplm_WindowVR, -1);
        XPLMSetWindowGeometryVR(g_win, 1008, 773);
    } else if (msg == XPLM_MSG_EXITING_VR) {
        XPLMSetWindowPositioningMode(g_win, xplm_WindowPositionFree, -1);
    }
}
