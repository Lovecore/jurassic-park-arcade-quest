/* plat_win.c — plat.h on Windows x64 (the development host) */
#if defined(_WIN32)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

uint64_t plat_mono_ns(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

uint64_t plat_thread_cpu_ns(void)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
            (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100u;
}

int64_t plat_real_ns(void)
{
    FILETIME ft;
    uint64_t t;
    GetSystemTimePreciseAsFileTime(&ft);
    t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;      /* 100 ns since 1601 */
    return (int64_t)(t - 116444736000000000ull) * 100;
}

void plat_sleep_ms(uint32_t ms) { Sleep(ms); }

void plat_timer_resolution(void)
{
    static int done;
    if (!done) { done = 1; timeBeginPeriod(1); }
}

int plat_wait32(volatile uint32_t *addr, uint32_t expected, int64_t timeout_ns)
{
    DWORD ms = timeout_ns < 0 ? INFINITE : (DWORD)(timeout_ns / 1000000);
    if (!WaitOnAddress((volatile void *)addr, &expected, 4, ms))
        return GetLastError() == ERROR_TIMEOUT ? 1 : 0;
    return 0;
}

void plat_wake32(volatile uint32_t *addr, int all)
{
    if (all) WakeByAddressAll((void *)addr); else WakeByAddressSingle((void *)addr);
}

struct start { void *(*fn)(void *); void *arg; };

static DWORD WINAPI thread_entry(LPVOID p)
{
    struct start s = *(struct start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

int plat_thread_start(void *(*fn)(void *), void *arg, size_t stack_bytes)
{
    struct start *s = malloc(sizeof(*s));
    HANDLE h;
    s->fn = fn;
    s->arg = arg;
    h = CreateThread(NULL, stack_bytes, thread_entry, s, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) { free(s); return -1; }
    CloseHandle(h);
    return 0;
}

void plat_thread_exit(void) { ExitThread(0); }
uint64_t plat_thread_id(void) { return GetCurrentThreadId(); }

uint8_t *plat_reserve(uint64_t bytes) { return VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_NOACCESS); }
int plat_commit(void *p, size_t bytes) { return VirtualAlloc(p, bytes, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1; }
void plat_decommit(void *p, size_t bytes) { VirtualFree(p, bytes, MEM_DECOMMIT); }

void plat_protect(void *p, size_t bytes, int writable)
{
    DWORD old;
    VirtualProtect(p, bytes, writable ? PAGE_READWRITE : PAGE_READONLY, &old);
}

void plat_exe_dir(char *out, size_t n)
{
    char *s;
    GetModuleFileNameA(NULL, out, (DWORD)n);
    for (s = out; *s; s++) if (*s == '\\') *s = '/';
    s = strrchr(out, '/');
    if (s) *s = 0;
}

int plat_is_dir(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

struct plat_dir { HANDLE h; WIN32_FIND_DATAA fd; int first; };

plat_dir *plat_opendir(const char *path)
{
    char pat[1100];
    plat_dir *d = malloc(sizeof(*d));
    snprintf(pat, sizeof(pat), "%s/*", path);
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1;
    return d;
}

const char *plat_readdir(plat_dir *d, int *is_dir)
{
    for (;;) {
        if (!d->first && !FindNextFileA(d->h, &d->fd)) return NULL;
        d->first = 0;
        if (!strcmp(d->fd.cFileName, ".") || !strcmp(d->fd.cFileName, "..")) continue;
        if (is_dir) *is_dir = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return d->fd.cFileName;
    }
}

void plat_closedir(plat_dir *d) { FindClose(d->h); free(d); }

int plat_rename(const char *from, const char *to) { return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1; }

static plat_fault_fn g_fault;

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    int kind, r;
    uintptr_t addr = 0;
    int write = 0;
    switch (er->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        kind = PLAT_FAULT_ACCESS;
        if (er->NumberParameters >= 2) { write = er->ExceptionInformation[0] == 1; addr = er->ExceptionInformation[1]; }
        break;
    case EXCEPTION_STACK_OVERFLOW: kind = PLAT_FAULT_STACK; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: kind = PLAT_FAULT_DIV0; break;
    case EXCEPTION_SINGLE_STEP: kind = PLAT_FAULT_STEP; break;
    default: return EXCEPTION_CONTINUE_SEARCH;
    }
    r = g_fault ? g_fault(kind, addr, write, er->ExceptionAddress) : 0;
    if (r == 2) ep->ContextRecord->EFlags |= 0x100;
    return r ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void plat_install_fault_handler(plat_fault_fn fn)
{
    g_fault = fn;
    AddVectoredExceptionHandler(1, veh);
}

void plat_backtrace(void (*line)(const char *))
{
    void *frames[48];
    USHORT n = RtlCaptureStackBackTrace(1, 48, frames, NULL), i;
    for (i = 0; i < n; i++) {
        HMODULE m = NULL;
        char name[MAX_PATH] = "?", buf[400];
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)frames[i], &m);
        if (m) GetModuleFileNameA(m, name, MAX_PATH);
        snprintf(buf, sizeof(buf), "  host frame %2u: %s+%#llx\n", i,
                 strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name,
                 (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)m));
        line(buf);
    }
}
#endif

#if defined(_WIN32)
void plat_guest_thread_init(void) {}
#endif

#if defined(_WIN32)
void plat_yield(void) { SwitchToThread(); }
#endif
