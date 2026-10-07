/* Headless mode and --record: headless.c. */
#ifndef UT99_HEADLESS_H
#define UT99_HEADLESS_H
#include "native32.h"

extern int g_headless;
extern const char* g_record;       /* --record out.mp4 */
extern int g_record_frames;        /* --frames N: stop after N recorded frames */

extern native32_shim_t g_headless_shims[];   /* only with --headless */
extern const int g_headless_nshims;
extern native32_shim_t g_record_shim;        /* BitBlt, with --record */

void headless_start(void);

#endif
