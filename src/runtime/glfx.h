#ifndef GLFX_H
#define GLFX_H
#include <stdint.h>
#include <windows.h>

extern int g_msaa, g_aniso;   /* --msaa N, --aniso N; 0 = the game's own */

/* GetProcAddress's answer for a native module, or a wrapper in its place. */
uint32_t glfx_proc(HMODULE h, const char* name, uint32_t real);

#endif
