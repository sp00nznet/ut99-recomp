#ifndef GLFX_H
#define GLFX_H
#include <stdint.h>
#include <windows.h>

extern int g_msaa, g_aniso;   /* --msaa N, --aniso N; 0 = the game's own */
extern int g_vsync, g_fps_cap, g_show_fps;   /* --vsync 0|1 (-1: the driver's), --fps-cap N, --show-fps */

/* GetProcAddress's answer for a native module, or a wrapper in its place. */
uint32_t glfx_proc(HMODULE h, const char* name, uint32_t real);

#endif
