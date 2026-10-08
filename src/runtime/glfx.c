/*
 * The makeover on UT's own OpenGL renderer (OpenGlDrv.dll, lifted): it finds
 * every GL, WGL and pixel-format function with GetProcAddress, so the host
 * hands it a wrapper for a few of them.
 *
 *   --msaa N    SetPixelFormat gets a multisample format with N samples
 *               (wglChoosePixelFormatARB), so every polygon edge is smoothed.
 *   --aniso N   every mipmapped texture is filtered trilinear and N-times
 *               anisotropic, instead of the driver's nearest-mip bilinear.
 *
 * Off unless asked for: the stock game keeps the original's behaviour.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "glfx.h"

int g_msaa, g_aniso;

typedef BOOL(WINAPI* set_pf_t)(HDC, int, const PIXELFORMATDESCRIPTOR*);
typedef void(WINAPI* texparami_t)(unsigned, unsigned, int);
typedef void(WINAPI* texparamf_t)(unsigned, unsigned, float);
typedef BOOL(WINAPI* choose_pf_arb_t)(HDC, const int*, const float*, UINT, int*, UINT*);

#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE

static set_pf_t real_set_pf;
static texparami_t real_texparami;
static texparamf_t real_texparamf;

/* ---- multisampling --------------------------------------------------------
 * wglChoosePixelFormatARB needs a current context to be found, and a window's
 * format is set once: so a throwaway window and context, once, to get it. */
static choose_pf_arb_t choose_arb(void) {
    static choose_pf_arb_t fn;
    static int tried;
    if (tried) return fn;
    tried = 1;
    HWND w = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 1, 1, NULL, NULL, NULL, NULL);
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                  PFD_TYPE_RGBA, 32 };
    HGLRC rc = NULL;
    if (SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd) && (rc = wglCreateContext(dc))) {
        HGLRC old_rc = wglGetCurrentContext();
        HDC old_dc = wglGetCurrentDC();
        wglMakeCurrent(dc, rc);
        fn = (choose_pf_arb_t)wglGetProcAddress("wglChoosePixelFormatARB");
        wglMakeCurrent(old_dc, old_rc);
        wglDeleteContext(rc);
    }
    ReleaseDC(w, dc);
    DestroyWindow(w);
    if (!fn) fprintf(stderr, "[glfx] no wglChoosePixelFormatARB: no multisampling\n");
    return fn;
}

static int g_ms_format;   /* the multisample format we gave a window */

static BOOL WINAPI fx_SetPixelFormat(HDC dc, int format, const PIXELFORMATDESCRIPTOR* pfd) {
    choose_pf_arb_t choose = g_msaa ? choose_arb() : NULL;
    if (choose && pfd) {
        if (g_ms_format && GetPixelFormat(dc) == g_ms_format) return TRUE;   /* a context made again */
        int attrs[] = {
            0x2001 /* DRAW_TO_WINDOW */, 1, 0x2010 /* SUPPORT_OPENGL */, 1, 0x2011 /* DOUBLE_BUFFER */, 1,
            0x2013 /* PIXEL_TYPE */, 0x202B /* RGBA */, 0x2014 /* COLOR_BITS */, 32,
            /* a minimum; UT asks for 32 and window formats stop at 24 */
            0x2022 /* DEPTH_BITS */, 24, 0x2023 /* STENCIL_BITS */, pfd->cStencilBits,
            0x2041 /* SAMPLE_BUFFERS */, 1, 0x2042 /* SAMPLES */, g_msaa, 0 };
        int ms;
        UINT n = 0;
        if (choose(dc, attrs, NULL, 1, &ms, &n) && n && real_set_pf(dc, ms, pfd)) {
            g_ms_format = ms;
            fprintf(stderr, "[glfx] pixel format %d: %dx multisampling (the game asked for %d)\n", ms, g_msaa, format);
            return TRUE;
        }
        fprintf(stderr, "[glfx] no %dx multisample format; the game's own\n", g_msaa);
    }
    return real_set_pf(dc, format, pfd);
}

/* ---- texture filtering ---------------------------------------------------- */
static void WINAPI fx_glTexParameteri(unsigned target, unsigned pname, int v) {
    if (pname == GL_TEXTURE_MIN_FILTER && (v == GL_LINEAR_MIPMAP_NEAREST || v == GL_NEAREST_MIPMAP_LINEAR)) {
        static int said;
        if (!said++) fprintf(stderr, "[glfx] textures: trilinear, %dx anisotropic\n", g_aniso);
        v = GL_LINEAR_MIPMAP_LINEAR;
        real_texparamf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)g_aniso);
    }
    real_texparami(target, pname, v);
}

static void WINAPI fx_glTexParameterf(unsigned target, unsigned pname, float v) {
    if (pname == GL_TEXTURE_MIN_FILTER) { fx_glTexParameteri(target, pname, (int)v); return; }
    real_texparamf(target, pname, v);
}

/* ---- the swap -------------------------------------------------------------
 * GetProcAddress from the guest: a wrapper in place of the real function,
 * which it keeps to call. */
uint32_t glfx_proc(HMODULE h, const char* name, uint32_t real) {
    if (!real || (uintptr_t)name < 0x10000) return real;
    if (g_msaa && !strcmp(name, "SetPixelFormat")) {
        real_set_pf = (set_pf_t)(uintptr_t)real;
        return (uint32_t)(uintptr_t)fx_SetPixelFormat;
    }
    if (g_aniso && !strcmp(name, "glTexParameteri")) {
        real_texparami = (texparami_t)(uintptr_t)real;
        if (!real_texparamf) real_texparamf = (texparamf_t)(uintptr_t)GetProcAddress(h, "glTexParameterf");
        return (uint32_t)(uintptr_t)fx_glTexParameteri;
    }
    if (g_aniso && !strcmp(name, "glTexParameterf")) {
        real_texparamf = (texparamf_t)(uintptr_t)real;
        if (!real_texparami) real_texparami = (texparami_t)(uintptr_t)GetProcAddress(h, "glTexParameteri");
        return (uint32_t)(uintptr_t)fx_glTexParameterf;
    }
    return real;
}
