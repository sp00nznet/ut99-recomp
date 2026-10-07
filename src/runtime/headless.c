/*
 * Headless mode and --record (REPO_RULES section 13: nothing on any screen,
 * works over RDP).
 *
 * The window: every top-level window the game creates is layered at alpha 0,
 * click-through, never activated and off the taskbar. Windows treats it as
 * visible, so the game paints into it as usual, and nothing reaches a screen
 * (Red Alert 2's host found a hidden window gets no WM_PAINT). The game's
 * mouse capture is kept off the real cursor.
 *
 * The frames: with DirectDraw off, WinDrv draws each frame by BitBlt from the
 * DIB section the render device wrote into. The BitBlt shim copies that DIB's
 * pixels, and a recorder thread sends the newest one to ffmpeg at a steady
 * 30 fps, whatever the game's frame rate is. --frames N stops after N.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "headless.h"

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define REC_FPS 30

int g_headless;
const char* g_record;
int g_record_frames;

static CRITICAL_SECTION g_frame_lock;
static uint32_t* g_frame;            /* newest frame, BGRA, top-down */
static int g_fw, g_fh;
static volatile LONG g_frame_count;  /* frames the game has drawn */

/* ---- the window ---------------------------------------------------------- */

#define HL_EXSTYLE (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)

static HWND g_game_hwnd;            /* the window the frames are blitted to: the viewport */

static void shim_CreateWindowExW(void) {
    uint32_t ex = ARG(0), style = ARG(3);
    int top = !(style & WS_CHILD);
    HWND h = CreateWindowExW(top ? ex | HL_EXSTYLE : ex, (LPCWSTR)(uintptr_t)ARG(1),
                             (LPCWSTR)(uintptr_t)ARG(2), style, (int)ARG(4), (int)ARG(5),
                             (int)ARG(6), (int)ARG(7), (HWND)(uintptr_t)ARG(8),
                             (HMENU)(uintptr_t)ARG(9), (HINSTANCE)(uintptr_t)ARG(10),
                             (LPVOID)(uintptr_t)ARG(11));
    if (h && top) SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 12 * 4;
}

static void shim_ClipCursor(void) { g_eax = TRUE; g_esp += 4 + 1 * 4; }
static void shim_SetCursorPos(void) { g_eax = TRUE; g_esp += 4 + 2 * 4; }
static void shim_SetForegroundWindow(void) { g_eax = TRUE; g_esp += 4 + 1 * 4; }

/* ---- the frames ---------------------------------------------------------- */

/* Blit what the game is about to put on its window into our own top-down
 * 32-bit DIB: GDI handles the source's orientation and pixel format (reading
 * the game's DIB by hand got the row order wrong, and the video came out
 * upside down). */
static HDC g_cap_dc;
static HBITMAP g_cap_bmp;
static uint32_t* g_cap_bits;
static int g_cap_w, g_cap_h;

static void grab(HDC src, int sx, int sy, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w != g_cap_w || h != g_cap_h) {
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
        if (g_cap_bmp) DeleteObject(g_cap_bmp);
        if (!g_cap_dc) g_cap_dc = CreateCompatibleDC(NULL);
        g_cap_bmp = CreateDIBSection(g_cap_dc, &bi, DIB_RGB_COLORS, (void**)&g_cap_bits, NULL, 0);
        if (!g_cap_bmp) { g_cap_w = g_cap_h = 0; return; }
        SelectObject(g_cap_dc, g_cap_bmp);
        g_cap_w = w, g_cap_h = h;
    }
    if (!BitBlt(g_cap_dc, 0, 0, w, h, src, sx, sy, SRCCOPY)) return;
    GdiFlush();
    EnterCriticalSection(&g_frame_lock);
    if (w != g_fw || h != g_fh) {
        free(g_frame);
        g_frame = (uint32_t*)malloc((size_t)w * h * 4);
        g_fw = w, g_fh = h;
    }
    memcpy(g_frame, g_cap_bits, (size_t)w * h * 4);
    LeaveCriticalSection(&g_frame_lock);
    InterlockedIncrement(&g_frame_count);
}

static void shim_BitBlt(void) {
    HDC src = (HDC)(uintptr_t)ARG(5);
    HWND to = WindowFromDC((HDC)(uintptr_t)ARG(0));
    if (to) g_game_hwnd = to;
    if (g_record && src) grab(src, (int)ARG(6), (int)ARG(7), (int)ARG(3), (int)ARG(4));
    g_eax = BitBlt((HDC)(uintptr_t)ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), src,
                   (int)ARG(6), (int)ARG(7), ARG(8));
    g_esp += 4 + 9 * 4;
}

static DWORD WINAPI recorder(LPVOID unused) {
    (void)unused;
    FILE* ff = NULL;
    int w = 0, h = 0, n = 0;
    uint32_t* buf = NULL;
    while (!g_frame_count) Sleep(10);
    for (DWORD next = GetTickCount();; next += 1000 / REC_FPS) {
        EnterCriticalSection(&g_frame_lock);
        if (!ff) {
            char cmd[MAX_PATH + 256];
            w = g_fw, h = g_fh;
            buf = (uint32_t*)malloc((size_t)w * h * 4);
            _snprintf(cmd, sizeof cmd - 1,
                      "ffmpeg -y -loglevel error -f rawvideo -pix_fmt bgra -s %dx%d -r %d -i - "
                      "-pix_fmt yuv420p -c:v libx264 -preset veryfast \"%s\"", w, h, REC_FPS, g_record);
            ff = _popen(cmd, "wb");
            if (!ff) { LeaveCriticalSection(&g_frame_lock); fprintf(stderr, "[record] cannot start ffmpeg\n"); return 0; }
            fprintf(stderr, "[record] %dx%d at %d fps to %s\n", w, h, REC_FPS, g_record);
        }
        if (g_fw == w && g_fh == h) memcpy(buf, g_frame, (size_t)w * h * 4);   /* else keep the last */
        LeaveCriticalSection(&g_frame_lock);
        fwrite(buf, 4, (size_t)w * h, ff);
        if (g_record_frames && ++n >= g_record_frames) {
            _pclose(ff);
            fprintf(stderr, "[record] %d frames written; the game drew %ld\n", n, g_frame_count);
            DeleteFileA("Running.ini");      /* a deliberate stop, not a crash */
            TerminateProcess(GetCurrentProcess(), 0);
        }
        DWORD now = GetTickCount();
        if ((int)(next + 1000 / REC_FPS - now) > 0) Sleep(next + 1000 / REC_FPS - now);
    }
}

native32_shim_t g_headless_shims[] = {
    { "CreateWindowExW", shim_CreateWindowExW },
    { "ClipCursor", shim_ClipCursor },
    { "SetCursorPos", shim_SetCursorPos },
    { "SetForegroundWindow", shim_SetForegroundWindow },
};
const int g_headless_nshims = sizeof g_headless_shims / sizeof g_headless_shims[0];
native32_shim_t g_record_shim = { "BitBlt", shim_BitBlt };

/* ---- scripted input ------------------------------------------------------
 * --key VK@S: press and release virtual key VK (decimal or 0x hex, or a name
 * below) S seconds after start, as window messages to the game's window, which
 * is how WinDrv reads the keyboard with DirectInput off. Headless only, so it
 * never reaches a window someone is typing into. */
typedef struct { uint32_t vk; double at; } key_t;
static key_t g_keys[64];
static int g_nkeys;

int headless_key_arg(const char* spec) {
    static const struct { const char* name; uint32_t vk; } names[] = {
        { "esc", VK_ESCAPE }, { "enter", VK_RETURN }, { "up", VK_UP }, { "down", VK_DOWN },
        { "left", VK_LEFT }, { "right", VK_RIGHT }, { "space", VK_SPACE }, { "tab", VK_TAB },
        { "fire", VK_LBUTTON }, { "altfire", VK_RBUTTON },
    };
    const char* at = strchr(spec, '@');
    if (!at || g_nkeys == 64) return 0;
    uint32_t vk = 0;
    for (int i = 0; i < (int)(sizeof names / sizeof *names); i++)
        if (!_strnicmp(spec, names[i].name, at - spec) && strlen(names[i].name) == (size_t)(at - spec))
            vk = names[i].vk;
    if (!vk) vk = strtoul(spec, NULL, 0);
    if (!vk) return 0;
    g_keys[g_nkeys].vk = vk;
    g_keys[g_nkeys++].at = atof(at + 1);
    return 1;
}

static DWORD WINAPI key_script(LPVOID unused) {
    (void)unused;
    DWORD t0 = GetTickCount();
    for (int i = 0; i < g_nkeys; i++) {
        DWORD due = t0 + (DWORD)(g_keys[i].at * 1000);
        while ((int)(due - GetTickCount()) > 0 || !g_game_hwnd) Sleep(10);
        if (g_keys[i].vk == VK_LBUTTON || g_keys[i].vk == VK_RBUTTON) {
            /* fire / altfire: a click in the middle of the viewport */
            int l = g_keys[i].vk == VK_LBUTTON;
            RECT rc;
            GetClientRect(g_game_hwnd, &rc);
            LPARAM at = MAKELPARAM(rc.right / 2, rc.bottom / 2);
            PostMessageW(g_game_hwnd, l ? WM_LBUTTONDOWN : WM_RBUTTONDOWN, l ? MK_LBUTTON : MK_RBUTTON, at);
            Sleep(60);
            PostMessageW(g_game_hwnd, l ? WM_LBUTTONUP : WM_RBUTTONUP, 0, at);
        } else {
            UINT sc = MapVirtualKeyA(g_keys[i].vk, MAPVK_VK_TO_VSC);
            PostMessageW(g_game_hwnd, WM_KEYDOWN, g_keys[i].vk, 1 | sc << 16);
            Sleep(60);
            PostMessageW(g_game_hwnd, WM_KEYUP, g_keys[i].vk, 1 | sc << 16 | 0xC0000000u);
        }
        fprintf(stderr, "[input] key 0x%02X at %.1f s\n", g_keys[i].vk, g_keys[i].at);
    }
    return 0;
}

void headless_start(void) {
    InitializeCriticalSection(&g_frame_lock);
    if (g_record) CloseHandle(CreateThread(NULL, 0, recorder, NULL, 0, NULL));
    if (g_nkeys) CloseHandle(CreateThread(NULL, 0, key_script, NULL, 0, NULL));
}
