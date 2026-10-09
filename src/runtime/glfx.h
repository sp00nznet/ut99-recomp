#ifndef GLFX_H
#define GLFX_H
#include <stdint.h>
#include <windows.h>

#define GLFX_UNSET (-1000)   /* a setting no flag gave: glfx_config reads the ini */

extern int g_msaa, g_aniso;   /* --msaa N, --aniso N; 0 = the game's own */
extern int g_vsync, g_fps_cap, g_show_fps;   /* --vsync 0|1 (-1: the driver's), --fps-cap N, --show-fps */

/* Fill the settings no flag gave from the game's ini; argv is the guest's
 * (argv[1..argc]), where INI= names it. Before the game starts. */
void glfx_config(const char* system_dir, int argc, char** argv);

/* GetProcAddress's answer for a native module, or a wrapper in its place. */
uint32_t glfx_proc(HMODULE h, const char* name, uint32_t real);

#endif
