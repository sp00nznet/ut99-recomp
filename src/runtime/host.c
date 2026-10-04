/*
 * Unreal Tournament (v436) - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and machine lock; see its header). UT is a launcher and a dozen DLLs, every
 * one of them lifted, so what is here is mostly the loader: each guest module
 * is mapped at the base tools/rebase.py gave it, bound, and attached (its
 * DllMain run) the first time the game loads it, the way Windows would.
 * docs/architecture.md has the reasoning.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so the guest's 0x00400000 (UCC)
 * and 0x10100000..0x11336000 (everything else) are free.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "recomp_trace.h"

/* ---- the guest modules ---------------------------------------------------
 * Same bases as tools/rebase.py: the lifted C has them baked in. A module
 * is attached (DllMain) when the launcher's imports need it or when the
 * engine LoadLibrary's it, after the guest modules it imports itself. */
typedef struct {
    const char* name;
    uint32_t    base;
    int         attached;
} module_t;

static module_t g_modules[] = {
    { "Core.dll",             0x10100000u },
    { "Engine.dll",           0x10300000u },
    { "Window.dll",           0x11000000u },
    { "Galaxy.dll",           0x10600000u },
    { "IpDrv.dll",            0x10700000u },
    { "Fire.dll",             0x10800000u },
    { "UWeb.dll",             0x10A00000u },
    { "Render.dll",           0x10B00000u },
    { "SoftDrv.dll",          0x10D00000u },
    { "WinDrv.dll",           0x11100000u },
    { "OpenGlDrv.dll",        0x11200000u },
    { "D3DDrv.dll",           0x11300000u, 0 },
    { "UnrealTournament.exe", 0x10900000u },
    { "UCC.exe",              0x00400000u },
};
#define NMODULES ((int)(sizeof g_modules / sizeof g_modules[0]))

static module_t* g_exe;               /* the launcher this run enters */
static char g_system_dir[MAX_PATH];   /* game\System, full path */
static char g_guest_exe[MAX_PATH];
static char g_guest_cmdline[1024];
static WCHAR g_guest_cmdline_w[1024];

static DWORD g_watchdog_s;
static int g_headless;

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

static module_t* module_by_base(uint32_t h) {
    for (int i = 0; i < NMODULES; i++)
        if (g_modules[i].base == h) return &g_modules[i];
    return NULL;
}

/* "WinDrv", "WinDrv.dll", "..\System\WinDrv.dll": the file part, with or
 * without its extension. Case-insensitive, like the loader. */
static module_t* module_by_name(const char* path) {
    const char* p = path;
    for (const char* s = path; *s; s++)
        if (*s == '\\' || *s == '/' || *s == ':') p = s + 1;
    size_t n = strlen(p);
    for (int i = 0; i < NMODULES; i++) {
        const char* m = g_modules[i].name;
        size_t k = strchr(m, '.') - m;
        if (!_stricmp(p, m) || (n == k && !_strnicmp(p, m, k))) return &g_modules[i];
    }
    return NULL;
}

static module_t* module_by_name_w(const WCHAR* path) {
    char a[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, path, -1, a, sizeof a, NULL, NULL);
    return module_by_name(a);
}

/* A DLL in the game's own System folder that is not lifted (Editor.dll, the
 * Glide driver): loading the original would run its native code against
 * native copies of Core and Engine, so it fails as a missing file instead. */
static int in_system_dir(const char* path) {
    char full[MAX_PATH], dir[MAX_PATH];
    char* file;
    GetFullPathNameA(path, MAX_PATH, full, &file);
    if (!file) return 0;
    _snprintf(dir, sizeof dir - 1, "%s\\%s", g_system_dir, file);
    return GetFileAttributesA(dir) != INVALID_FILE_ATTRIBUTES;
}

/* Run a DLL's entry point once, DLL_PROCESS_ATTACH, as the loader would:
 * the guest modules it imports first (Core before everything, since every
 * other module's static constructors call into it). */
static int attach(module_t* m) {
    if (m->attached) return 1;
    m->attached = 1;
    uint8_t* b = (uint8_t*)(uintptr_t)m->base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(b + dd.VirtualAddress);
         dd.VirtualAddress && d->Name; d++) {
        module_t* dep = module_by_name((const char*)(b + d->Name));
        if (dep && !attach(dep)) return 0;
    }
    if (nt->FileHeader.Characteristics & IMAGE_FILE_DLL && nt->OptionalHeader.AddressOfEntryPoint) {
        uint32_t args[3] = { m->base, DLL_PROCESS_ATTACH, 0 };
        fprintf(stderr, "[load] %s: DllMain\n", m->name);
        native32_call_guest(m->base + nt->OptionalHeader.AddressOfEntryPoint, 3, args);
        if (!g_eax) { fprintf(stderr, "[load] %s: DllMain failed\n", m->name); return 0; }
    }
    return 1;
}

/* ---- the loader's shims --------------------------------------------------
 * Every one runs inside the lifted model: arguments from g_esp, and it pops
 * its own (stdcall). */

static uint32_t load_guest(module_t* m) {
    if (!m) return 0;
    if (!attach(m)) return 0;
    return m->base;
}

static uint32_t load_native_a(const char* name) {
    if (in_system_dir(name)) {
        fprintf(stderr, "[load] %s: not lifted, reported missing\n", name);
        SetLastError(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    return (uint32_t)(uintptr_t)LoadLibraryA(name);
}

static void shim_LoadLibraryA(void) {
    const char* name = (const char*)(uintptr_t)ARG(0);
    module_t* m = module_by_name(name);
    g_eax = m ? load_guest(m) : load_native_a(name);
    g_esp += 4 + 1 * 4;
}

static void shim_LoadLibraryW(void) {
    const WCHAR* name = (const WCHAR*)(uintptr_t)ARG(0);
    char a[MAX_PATH];
    module_t* m = module_by_name_w(name);
    WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof a, NULL, NULL);
    g_eax = m ? load_guest(m) : load_native_a(a);
    g_esp += 4 + 1 * 4;
}

static void shim_FreeLibrary(void) {
    g_eax = module_by_base(ARG(0)) ? TRUE : FreeLibrary((HMODULE)(uintptr_t)ARG(0));
    g_esp += 4 + 1 * 4;
}

/* By name an export of a guest module is its VA in the dispatch table (code)
 * or in its data (UT's `int<Class>exec<Func>` native pointers and
 * `autoclass<Class>` registrations are data exports). */
static void shim_GetProcAddress(void) {
    uint32_t h = ARG(0);
    const char* name = (const char*)(uintptr_t)ARG(1);
    g_eax = module_by_base(h) ? native32_export(h, name)
                              : (uint32_t)(uintptr_t)GetProcAddress((HMODULE)(uintptr_t)h, name);
    g_esp += 4 + 2 * 4;
}

static uint32_t module_handle(module_t* m, int native_ok, uint32_t native) {
    if (m) return m->base;
    return native_ok ? native : 0;
}

static void shim_GetModuleHandleA(void) {
    const char* name = (const char*)(uintptr_t)ARG(0);
    g_eax = !name ? g_exe->base
                  : module_handle(module_by_name(name), 1, (uint32_t)(uintptr_t)GetModuleHandleA(name));
    g_esp += 4 + 1 * 4;
}

static void shim_GetModuleHandleW(void) {
    const WCHAR* name = (const WCHAR*)(uintptr_t)ARG(0);
    g_eax = !name ? g_exe->base
                  : module_handle(module_by_name_w(name), 1, (uint32_t)(uintptr_t)GetModuleHandleW(name));
    g_esp += 4 + 1 * 4;
}

/* game\System\<module>: UT finds its base directory (and every package and
 * .ini) from where its exe and DLLs say they are. */
static int guest_path(uint32_t h, char* out, size_t n) {
    module_t* m = h ? module_by_base(h) : g_exe;
    if (!m) return 0;
    _snprintf(out, n - 1, "%s\\%s", g_system_dir, m->name);
    out[n - 1] = 0;
    return 1;
}

static void shim_GetModuleFileNameA(void) {
    char path[MAX_PATH];
    char* out = (char*)(uintptr_t)ARG(1);
    uint32_t size = ARG(2);
    if (guest_path(ARG(0), path, sizeof path)) {
        uint32_t len = (uint32_t)strlen(path);
        if (size) {
            uint32_t k = len < size ? len : size - 1;
            memcpy(out, path, k);
            out[k] = 0;
            len = k;
        }
        g_eax = len;
    } else {
        g_eax = GetModuleFileNameA((HMODULE)(uintptr_t)ARG(0), out, size);
    }
    g_esp += 4 + 3 * 4;
}

static void shim_GetModuleFileNameW(void) {
    char path[MAX_PATH];
    WCHAR* out = (WCHAR*)(uintptr_t)ARG(1);
    uint32_t size = ARG(2);
    if (guest_path(ARG(0), path, sizeof path)) {
        WCHAR w[MAX_PATH];
        uint32_t len = (uint32_t)MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH) - 1;
        if (size) {
            uint32_t k = len < size ? len : size - 1;
            memcpy(out, w, k * sizeof(WCHAR));
            out[k] = 0;
            len = k;
        }
        g_eax = len;
    } else {
        g_eax = GetModuleFileNameW((HMODULE)(uintptr_t)ARG(0), out, size);
    }
    g_esp += 4 + 3 * 4;
}

/* __getmainargs(&argc, &argv, &envp, wildcard, &startinfo), cdecl: UCC's
 * main() takes argv from msvcrt, which parses the host's own command line. */
static int g_guest_argc;
static char* g_guest_argv[64];
static void shim_getmainargs(void) {
    MEM32(ARG(0)) = (uint32_t)g_guest_argc;
    MEM32(ARG(1)) = (uint32_t)(uintptr_t)g_guest_argv;
    MEM32(ARG(2)) = (uint32_t)(uintptr_t)_environ;
    g_eax = 0;
    g_esp += 4;
}

static void shim_GetCommandLineA(void) { g_eax = (uint32_t)(uintptr_t)g_guest_cmdline; g_esp += 4; }
static void shim_GetCommandLineW(void) { g_eax = (uint32_t)(uintptr_t)g_guest_cmdline_w; g_esp += 4; }

/* Headless: a message box prints and answers No/OK instead of waiting. */
static void shim_MessageBoxA(void) {
    uint32_t b = ARG(3) & MB_TYPEMASK;
    g_eax = (b == MB_YESNO || b == MB_YESNOCANCEL) ? IDNO : IDOK;
    fprintf(stderr, "[messagebox] %s: %s\n", (char*)(uintptr_t)ARG(2), (char*)(uintptr_t)ARG(1));
    g_esp += 4 + 4 * 4;
}

static void shim_MessageBoxW(void) {
    uint32_t b = ARG(3) & MB_TYPEMASK;
    g_eax = (b == MB_YESNO || b == MB_YESNOCANCEL) ? IDNO : IDOK;
    fprintf(stderr, "[messagebox] %ls: %ls\n", (WCHAR*)(uintptr_t)ARG(2), (WCHAR*)(uintptr_t)ARG(1));
    g_esp += 4 + 4 * 4;
}

static native32_shim_t g_shims[] = {
    { "LoadLibraryA", shim_LoadLibraryA },
    { "LoadLibraryW", shim_LoadLibraryW },
    { "FreeLibrary", shim_FreeLibrary },
    { "GetProcAddress", shim_GetProcAddress },
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleHandleW", shim_GetModuleHandleW },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetModuleFileNameW", shim_GetModuleFileNameW },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "GetCommandLineW", shim_GetCommandLineW },
    { "__getmainargs", shim_getmainargs },
    /* headless only: main() drops these otherwise */
    { "MessageBoxA", shim_MessageBoxA },
    { "MessageBoxW", shim_MessageBoxW },
};
#define NSHIMS_ALWAYS 11

/* ---- diagnostics --------------------------------------------------------- */

#define MAX_PROBES 8
static uint32_t g_probe[MAX_PROBES];
static int g_nprobe;
static volatile LONG g_probe_hits[MAX_PROBES];

/* --probe VA: report indirect calls to VA (RECOMP_ICALL asks this first). */
recomp_func_t recomp_lookup_manual(uint32_t va) {
    for (int i = 0; i < g_nprobe; i++)
        if (g_probe[i] == va && InterlockedIncrement(&g_probe_hits[i]) <= 5)
            fprintf(stderr, "[probe] sub_%08X from sub_%08X  ecx=%08X  args %08X %08X %08X\n",
                    va, g_cur_func, g_ecx, MEM32(g_esp), MEM32(g_esp + 4), MEM32(g_esp + 8));
    return NULL;
}

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr, "\n[not-lifted] sub_%08X  (called from 0x%08X)\n", va, g_cur_func);
    recomp_dump_trace("not-lifted");
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
}

static const char* module_of(uint32_t va) {
    for (int i = 0; i < NMODULES; i++) {
        uint8_t* b = (uint8_t*)(uintptr_t)g_modules[i].base;
        if (va < g_modules[i].base || !native32_in_guest(g_modules[i].base)) continue;
        IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
        if (va < g_modules[i].base + nt->OptionalHeader.SizeOfImage) return g_modules[i].name;
    }
    return "";
}

/* Added after native32's own handler, so callbacks are resolved first and
 * only real faults get here. Written with WriteFile from a static buffer: a
 * fault while another thread holds stderr's lock otherwise reports nothing. */
static char g_crash_buf[4096];
static int g_crash_len;
static void crash_emit(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(g_crash_buf + g_crash_len, sizeof g_crash_buf - 1 - g_crash_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_crash_len += n;
}

static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    /* 0xE06D7363 is a C++ throw: UT's appErrorf ends in one. */
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u && r->ExceptionCode != 0xE06D7363u)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&once, 1)) TerminateProcess(GetCurrentProcess(), 3);
    crash_emit("\n=== %s 0x%08lX at 0x%p, thread %lu ===\n",
               r->ExceptionCode == 0xE06D7363u ? "C++ throw" : "fault", r->ExceptionCode,
               r->ExceptionAddress, GetCurrentThreadId());
    /* UT throws its error text: `throw(const TCHAR*)` from appErrorf. The
     * object is a pointer to the string. */
    if (r->ExceptionCode == 0xE06D7363u && r->NumberParameters >= 3 && r->ExceptionInformation[1]) {
        uint32_t msg = *(uint32_t*)r->ExceptionInformation[1];
        if (native32_in_guest(msg) || msg > 0x10000)
            crash_emit("  thrown: %.400ls\n", (const wchar_t*)(uintptr_t)msg);
    }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        crash_emit("  %s of 0x%08X %s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at, module_of(at));
    }
    crash_emit("  in lifted sub_%08X (%s), last native call %s\n", g_cur_func, module_of(g_cur_func),
               g_cur_import);
    crash_emit("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
               g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    crash_emit("last indirect calls (newest first):\n");
    for (int i = 1; i <= 12 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char* nm = native32_name(g_icall_trace[k]);
        crash_emit("  0x%08X  from 0x%08X  %s%s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "",
                   nm ? "" : module_of(g_icall_trace[k]));
    }
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), g_crash_buf, (DWORD)g_crash_len, &w, NULL);
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X (%s), last native call %s, %u indirect calls\n",
            g_watchdog_s, g_cur_func, module_of(g_cur_func), g_cur_import, g_icall_count);
    native32_dump_icalls(8);
    recomp_trace_flush();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

int main(int argc, char** argv) {
    const char* game = "game";
    const char* launcher = "UnrealTournament.exe";
    char rebased[MAX_PATH] = "work\\rebased", game_full[MAX_PATH], tail[768] = "";
    int run = 0;
    /* stderr unbuffered: the log is the evidence, and the game leaves through
     * ExitProcess, which flushes nothing with the C runtime linked in (/MT). */
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--ucc")) launcher = "UCC.exe";
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--rebased") && i + 1 < argc) strcpy_s(rebased, sizeof rebased, argv[++i]);
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--probe") && i + 1 < argc && g_nprobe < MAX_PROBES)
            g_probe[g_nprobe++] = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else if (!strcmp(argv[i], "--")) {
            /* everything after -- is the game's own command line */
            for (i++; i < argc; i++) {
                strcat_s(tail, sizeof tail, " ");
                strcat_s(tail, sizeof tail, argv[i]);
                if (g_guest_argc < 62) g_guest_argv[++g_guest_argc] = argv[i];
            }
        } else {
            printf("usage: ut99 [--run] [--ucc] [--headless] [--watchdog S] [--game game] [--rebased work\\rebased]\n"
                   "            [--probe VA] [--native-trace] [--callbacks] [-- game arguments]\n"
                   "  ut99 --run                                   the game\n"
                   "  ut99 --run --ucc -- server DM-Deck16][        UCC: a dedicated server, no window\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(game, MAX_PATH, game_full, NULL);
    _snprintf(g_system_dir, sizeof g_system_dir - 1, "%s\\System", game_full);
    g_exe = module_by_name(launcher);
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%s\\%s", g_system_dir, launcher);
    g_guest_argv[0] = g_guest_exe;
    g_guest_argc += 1;
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"%s", g_guest_exe, tail);
    MultiByteToWideChar(CP_ACP, 0, g_guest_cmdline, -1, g_guest_cmdline_w, 1024);
    /* The engine runs from game\System, as UnrealTournament.exe does. Native
     * DLLs it asks for by name bind from there. */
    SetDllDirectoryA(g_system_dir);

    native32_init();
    AddVectoredExceptionHandler(0, crash);
    printf("Unreal Tournament recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    /* Map everything, then bind: a module's imports from another guest
     * module bind to that module's exports, so all must be mapped first. */
    for (int i = 0; i < NMODULES; i++) {
        char path[MAX_PATH];
        module_t* m = &g_modules[i];
        if (!strcmp(m->name + strlen(m->name) - 4, ".exe") && m != g_exe) continue;
        _snprintf(path, sizeof path - 1, "%s\\%s", rebased, m->name);
        uint32_t span = native32_map(path, m->base);
        if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", path, m->base); return 1; }
        printf("  mapped %-22s 0x%08X-0x%08X\n", m->name, m->base, m->base + span);
    }
    int nshims = g_headless ? (int)(sizeof g_shims / sizeof g_shims[0]) : NSHIMS_ALWAYS;
    int missing = 0;
    for (int i = 0; i < NMODULES; i++)
        if (native32_in_guest(g_modules[i].base)) missing += native32_bind(g_modules[i].base, g_shims, nshims);
    if (missing) fprintf(stderr, "warning: %d imports unresolved\n", missing);

    uint8_t* b = (uint8_t*)(uintptr_t)g_exe->base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    uint32_t entry = g_exe->base + nt->OptionalHeader.AddressOfEntryPoint;
    printf("  guest %s\n", g_guest_cmdline);
    if (!run) {
        printf("\n(dry run: mapped and bound; --run enters 0x%08X)\n", entry);
        return 0;
    }
    if (!SetCurrentDirectoryA(g_system_dir)) { fprintf(stderr, "cannot enter %s\n", g_system_dir); return 1; }
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    /* The launcher's own imports attach before it runs (attach() skips an
     * exe's entry point and runs its dependencies' DllMains). */
    if (!attach(g_exe)) return 1;
    printf("  entering 0x%08X\n\n", entry);
    fflush(stdout);
    native32_call_guest(entry, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
