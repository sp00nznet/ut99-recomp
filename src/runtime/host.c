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
#include <intrin.h>

#include "native32.h"
#include "recomp_trace.h"
#include "headless.h"
#include "glfx.h"

/* ---- the guest modules ---------------------------------------------------
 * Same bases as tools/rebase.py: the lifted C has them baked in. A module
 * is attached (DllMain) when the launcher's imports need it or when the
 * engine LoadLibrary's it, after the guest modules it imports itself. */
typedef struct {
    const char* name;
    uint32_t    base;
    int         attached;
    int         native;     /* --native: its original code runs */
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
static const char* g_lin_file;   /* --lift-in-native */

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
        uint32_t args[3] = { m->base, DLL_PROCESS_ATTACH, 0 }, ok;
        uint32_t entry = m->base + nt->OptionalHeader.AddressOfEntryPoint;
        fprintf(stderr, "[load] %s: DllMain%s\n", m->name, m->native ? " (original code)" : "");
        if (m->native) {
            ok = ((BOOL (WINAPI*)(HINSTANCE, DWORD, LPVOID))(uintptr_t)entry)(
                (HINSTANCE)(uintptr_t)m->base, DLL_PROCESS_ATTACH, NULL);
        } else {
            native32_call_guest(entry, 3, args);
            ok = g_eax;
        }
        if (!ok) { fprintf(stderr, "[load] %s: DllMain failed\n", m->name); return 0; }
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
                              : glfx_proc((HMODULE)(uintptr_t)h, name,
                                          (uint32_t)(uintptr_t)GetProcAddress((HMODULE)(uintptr_t)h, name));
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

/* --log: the engine's log on stderr as it is written. Every module logs
 * through Core's exported FOutputDevice::Logf (cdecl: this, [EName,] fmt,
 * ...), so its import reaches recomp_lookup_manual, which hands back these
 * wrappers: they run as the callee, print, and run the real Logf. UT's own
 * log file is flushed only at exit, and a killed run leaves it empty. */
#define CORE_LOGF       0x1010284Cu   /* ?Logf@FOutputDevice@@QAAXPBGZZ */
#define CORE_LOGF_EVENT 0x10102AD1u   /* ?Logf@FOutputDevice@@QAAXW4EName@@PBGZZ */
static int g_log;

static void log_line(uint32_t fmt_slot) {
    wchar_t buf[1024];
    const wchar_t* fmt = (const wchar_t*)(uintptr_t)MEM32(fmt_slot);
    if (!fmt) return;
    _vsnwprintf(buf, 1023, fmt, (va_list)(uintptr_t)(fmt_slot + 4));
    buf[1023] = 0;
    fprintf(stderr, "[log] %ls\n", buf);
}

static void hook_logf(void) {
    log_line(g_esp + 8);                      /* [ret][this][fmt] */
    recomp_lookup(CORE_LOGF)();
}

static void hook_logf_event(void) {
    log_line(g_esp + 12);                     /* [ret][this][event][fmt] */
    recomp_lookup(CORE_LOGF_EVENT)();
}

/* --probe VA: report indirect calls to VA (RECOMP_ICALL asks this first). */
recomp_func_t recomp_lookup_manual(uint32_t va) {
    if (g_log && va == CORE_LOGF) return hook_logf;
    if (g_log && va == CORE_LOGF_EVENT) return hook_logf_event;
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
        if (op == 8 && !IsBadReadPtr((void*)(uintptr_t)at, 16)) {
            MEMORY_BASIC_INFORMATION mbi;
            VirtualQuery((void*)(uintptr_t)at, &mbi, sizeof mbi);
            crash_emit("  bytes there:");
            for (int i = 0; i < 16; i++) crash_emit(" %02X", ((uint8_t*)(uintptr_t)at)[i]);
            crash_emit("\n  region 0x%p+0x%lX protect 0x%lX type 0x%lX\n", mbi.AllocationBase,
                       (unsigned long)mbi.RegionSize, mbi.Protect, mbi.Type);
        }
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

/* Code the guest builds at run time runs natively. Galaxy copies its mixer
 * templates out of its .text into GlobalAlloc memory, patches them, and calls
 * the copies: on 1999's Windows heap memory was executable, under DEP the
 * call faults. A static lift cannot hold code patched at run time, but the
 * copy is plain x86 working on guest memory in this address space, so an
 * execute fault in private (heap) memory makes that region executable and
 * carries on. A call from it back into a guest module still faults on the
 * module's non-executable code and goes through native32's trampoline.
 * ponytail: per title for now; a native32 option if a second title needs it. */
static LONG CALLBACK guest_generated_code(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    MEMORY_BASIC_INFORMATION mbi;
    DWORD old;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 ||
        r->ExceptionInformation[0] != 8)
        return EXCEPTION_CONTINUE_SEARCH;
    void* at = (void*)r->ExceptionInformation[1];
    if (native32_in_guest((uint32_t)(uintptr_t)at) || !VirtualQuery(at, &mbi, sizeof mbi) ||
        mbi.Type != MEM_PRIVATE || mbi.State != MEM_COMMIT || mbi.Protect != PAGE_READWRITE)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &old))
        return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "[gencode] guest-built code at 0x%p: 0x%p+0x%lX made executable\n", at,
            mbi.BaseAddress, (unsigned long)mbi.RegionSize);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X (%s), last native call %s, %u indirect calls\n",
            g_watchdog_s, g_cur_func, module_of(g_cur_func), g_cur_import, g_icall_count);
    native32_dump_icalls(8);
    {
        /* UT's clock: rdtsc cycles times the GSecondsPerCycle it measured at
         * startup, against the TSC rate measured here. */
        uint32_t spc = native32_export(0x10100000u, "?GSecondsPerCycle@@3MA");
        LARGE_INTEGER f, q0, q1;
        uint64_t t0 = __rdtsc();
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q0);
        Sleep(200);
        QueryPerformanceCounter(&q1);
        double hz = (double)(__rdtsc() - t0) * f.QuadPart / (double)(q1.QuadPart - q0.QuadPart);
        if (spc) fprintf(stderr, "[clock] GSecondsPerCycle %g (%.0f MHz); TSC measured %.0f MHz\n",
                         *(float*)(uintptr_t)spc, 1e-6 / *(float*)(uintptr_t)spc, hz * 1e-6);
    }
    recomp_trace_flush();
    /* The launcher marks a session with Running.ini and deletes it on a clean
     * exit; left behind, the next start opens Recovery Mode. A watchdog stop
     * is deliberate, so it is not a crash to recover from. */
    DeleteFileA("Running.ini");
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
        else if (!strcmp(argv[i], "--log")) g_log = 1;
        else if (!strcmp(argv[i], "--lift-in-native") && i + 1 < argc) {
            static char lin[MAX_PATH];             /* the run enters game\System */
            GetFullPathNameA(argv[++i], MAX_PATH, lin, NULL);
            g_lin_file = lin;
        }
        else if (!strcmp(argv[i], "--native") && i + 1 < argc) {
            module_t* m = module_by_name(argv[++i]);
            if (!m) { fprintf(stderr, "--native: no module %s\n", argv[i]); return 1; }
            m->native = 1;
        }
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--rebased") && i + 1 < argc) strcpy_s(rebased, sizeof rebased, argv[++i]);
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            static char rec[MAX_PATH];             /* the run enters game\System */
            GetFullPathNameA(argv[++i], MAX_PATH, rec, NULL);
            g_record = rec;
        }
        else if (!strcmp(argv[i], "--msaa") && i + 1 < argc) g_msaa = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--aniso") && i + 1 < argc) g_aniso = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--vsync") && i + 1 < argc) g_vsync = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fps-cap") && i + 1 < argc) g_fps_cap = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--show-fps")) g_show_fps = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_record_frames = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            if (!headless_key_arg(argv[++i])) { fprintf(stderr, "--key wants VK@seconds: %s\n", argv[i]); return 1; }
        }
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
            printf("usage: ut99 [--run] [--ucc] [--headless] [--record out.mp4] [--frames N] [--watchdog S]\n"
                   "            [--game game] [--rebased work\\rebased]\n"
                   "            [--log] [--native Module.dll] [--probe VA] [--native-trace] [--callbacks] [-- game arguments]\n"
                   "            [--msaa N] [--aniso N]   OpenGL renderer: N-sample anti-aliasing, N-times anisotropic\n"
                   "            [--vsync 0|1] [--fps-cap N] [--show-fps]   OpenGL renderer: frame pacing\n"
                   "  ut99 --run                                   the game\n"
                   "  ut99 --run --headless --record out.mp4 --frames 300   no window; 10 s of video\n"
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
    /* Room for the crash report on this thread when the stack itself is what
     * ran out: without it a stack overflow ends the process with no report. */
    {
        ULONG guarantee = 256 * 1024;
        SetThreadStackGuarantee(&guarantee);
    }
    AddVectoredExceptionHandler(1, guest_generated_code);
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
        printf("  mapped %-22s 0x%08X-0x%08X%s\n", m->name, m->base, m->base + span,
               m->native ? "  (original code)" : "");
        if (m->native) native32_set_native(m->base);
    }
    /* --lift-in-native FILE: functions of the --native modules that still run
     * lifted, one hex VA per line (tools/bisect.py writes it). */
    if (g_lin_file) {
        static uint32_t vas[16384];
        int n = 0;
        char line[64];
        FILE* f = fopen(g_lin_file, "r");
        if (!f) { fprintf(stderr, "cannot read %s\n", g_lin_file); return 1; }
        while (n < 16384 && fgets(line, sizeof line, f))
            if (line[0] == '0') vas[n++] = strtoul(line, NULL, 16);
        fclose(f);
        printf("  lifted inside native modules: %d of %d listed\n", native32_lift_in_native(vas, n), n);
    }
    /* One array for every module (native32_bind needs the same one each
     * time): the loader shims, then headless's and --record's. */
    static native32_shim_t shims[64];
    int nshims = 0;
    for (int i = 0; i < (g_headless ? (int)(sizeof g_shims / sizeof g_shims[0]) : NSHIMS_ALWAYS); i++)
        shims[nshims++] = g_shims[i];
    for (int i = 0; g_headless && i < g_headless_nshims; i++) shims[nshims++] = g_headless_shims[i];
    if (g_record || g_headless) shims[nshims++] = g_record_shim;   /* BitBlt: frames, and the viewport for --key */
    int missing = 0;
    for (int i = 0; i < NMODULES; i++)
        if (native32_in_guest(g_modules[i].base)) missing += native32_bind(g_modules[i].base, shims, nshims);
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
    headless_start();
    /* The launcher's own imports attach before it runs (attach() skips an
     * exe's entry point and runs its dependencies' DllMains). */
    if (!attach(g_exe)) return 1;
    printf("  entering 0x%08X\n\n", entry);
    fflush(stdout);
    native32_call_guest(entry, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
