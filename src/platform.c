/* All OS-specific code: POSIX (macOS, Linux) and Win32 behind #ifdef. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* pipe2, accept4, SOCK_CLOEXEC, getrandom */
#endif

#include "platform.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Bound for paths built while walking directory trees. */
#define PLAT_PATH_MAX 1024

int plat_path_join(char *out, size_t cap, const char *a, const char *b)
{
    size_t la = strlen(a);
    int n;
    if (la > 0 && (a[la - 1] == '/' || a[la - 1] == PLAT_SEP))
        n = snprintf(out, cap, "%s%s", a, b);
    else
        n = snprintf(out, cap, "%s%c%s", a, PLAT_SEP, b);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int64_t plat_unix_time(void)
{
    return (int64_t)time(NULL);
}

/* Strip the last path component in place (the executable name). */
static void strip_last_component(char *path)
{
    char *slash = strrchr(path, PLAT_SEP);
#ifdef _WIN32
    char *fwd = strrchr(path, '/');
    if (fwd && (!slash || fwd > slash))
        slash = fwd;
#endif
    if (slash)
        *slash = '\0';
}

#ifdef _WIN32
/* ======================================================================= */
/* Win32                                                                   */
/* ======================================================================= */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#endif

/* UTF-8 -> UTF-16 (malloc'd). The whole app speaks UTF-8 paths. */
static wchar_t *to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (!w)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
}

static int from_wide(const wchar_t *w, char *out, size_t cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    return n > 0 ? 0 : -1;
}

/* Owner-only DACL so tokens/credentials are not readable by other users. */
static int owner_only_sa(SECURITY_ATTRIBUTES *sa)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)", SDDL_REVISION_1, &sd, NULL))
        return -1;
    sa->nLength = sizeof *sa;
    sa->lpSecurityDescriptor = sd;
    sa->bInheritHandle = FALSE;
    return 0;
}

static char g_exe_dir[PLAT_PATH_MAX];
static INIT_ONCE g_exe_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK exe_dir_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    wchar_t mod[PLAT_PATH_MAX], fin[PLAT_PATH_MAX];
    DWORD n = GetModuleFileNameW(NULL, mod, PLAT_PATH_MAX);
    if (n == 0 || n >= PLAT_PATH_MAX)
        return TRUE;
    const wchar_t *best = mod;
    /* Resolve symlinks/junctions to the real location. */
    HANDLE h = CreateFileW(mod, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD m = GetFinalPathNameByHandleW(h, fin, PLAT_PATH_MAX, FILE_NAME_NORMALIZED);
        CloseHandle(h);
        if (m > 0 && m < PLAT_PATH_MAX) {
            if (!wcsncmp(fin, L"\\\\?\\UNC\\", 8)) {
                fin[6] = L'\\'; /* \\?\UNC\srv\share -> \\srv\share */
                best = fin + 6;
            } else if (!wcsncmp(fin, L"\\\\?\\", 4)) {
                best = fin + 4;
            } else {
                best = fin;
            }
        }
    }
    char tmp[PLAT_PATH_MAX];
    if (from_wide(best, tmp, sizeof tmp) != 0)
        return TRUE;
    strip_last_component(tmp);
    int k = snprintf(g_exe_dir, sizeof g_exe_dir, "%s", tmp);
    if (k < 0 || (size_t)k >= sizeof g_exe_dir)
        g_exe_dir[0] = '\0'; /* a cut-off path names the wrong folder: fail */
    return TRUE;
}

const char *plat_exe_dir(void)
{
    InitOnceExecuteOnce(&g_exe_once, exe_dir_init, NULL, NULL);
    return g_exe_dir[0] ? g_exe_dir : NULL;
}

int plat_mkdir(const char *path)
{
    wchar_t *w = to_wide(path);
    if (!w)
        return -1;
    SECURITY_ATTRIBUTES sa;
    int have_sa = owner_only_sa(&sa) == 0;
    int rc = 0;
    if (!CreateDirectoryW(w, have_sa ? &sa : NULL)) {
        DWORD attr = GetFileAttributesW(w); /* "already exists" is fine if it is a dir */
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
            rc = -1;
    }
    if (have_sa)
        LocalFree(sa.lpSecurityDescriptor);
    free(w);
    return rc;
}

int plat_write_file_atomic(const char *path, const void *data, size_t len)
{
    char tmp[PLAT_PATH_MAX];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmp || len > 0x7fffffff)
        return -1;
    wchar_t *wtmp = to_wide(tmp), *wdst = to_wide(path);
    int rc = -1;
    SECURITY_ATTRIBUTES sa;
    int have_sa = 0;
    if (!wtmp || !wdst)
        goto out;
    have_sa = owner_only_sa(&sa) == 0;
    HANDLE h = CreateFileW(wtmp, GENERIC_WRITE, 0, have_sa ? &sa : NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        goto out;
    DWORD written = 0;
    BOOL ok = len == 0 || (WriteFile(h, data, (DWORD)len, &written, NULL) && written == len);
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok && MoveFileExW(wtmp, wdst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        rc = 0;
    else
        DeleteFileW(wtmp);
out:
    if (have_sa)
        LocalFree(sa.lpSecurityDescriptor);
    free(wtmp);
    free(wdst);
    return rc;
}

char *plat_read_file(const char *path, size_t max_len, size_t *out_len)
{
    wchar_t *w = to_wide(path);
    if (!w)
        return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    LARGE_INTEGER sz;
    char *buf = NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || (unsigned long long)sz.QuadPart > max_len)
        goto out;
    buf = malloc((size_t)sz.QuadPart + 1);
    if (!buf)
        goto out;
    DWORD got = 0;
    if (sz.QuadPart > 0 && (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) ||
                            got != (DWORD)sz.QuadPart)) {
        free(buf);
        buf = NULL;
        goto out;
    }
    buf[got] = '\0';
    if (out_len)
        *out_len = got;
out:
    CloseHandle(h);
    return buf;
}

static int remove_tree_w(const wchar_t *w)
{
    DWORD attr = GetFileAttributesW(w);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? 0 : -1;
    }
    if (attr & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(w, attr & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
        return DeleteFileW(w) ? 0 : -1;
    if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) { /* never descend into junctions */
        size_t lw = wcslen(w);
        if (lw + 3 >= PLAT_PATH_MAX)
            return -1;
        wchar_t pat[PLAT_PATH_MAX];
        swprintf(pat, PLAT_PATH_MAX, L"%ls\\*", w);
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(pat, &fd);
        if (f != INVALID_HANDLE_VALUE) {
            int rc = 0;
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                    continue;
                wchar_t child[PLAT_PATH_MAX];
                int n = swprintf(child, PLAT_PATH_MAX, L"%ls\\%ls", w, fd.cFileName);
                if (n < 0 || remove_tree_w(child) != 0)
                    rc = -1;
            } while (FindNextFileW(f, &fd));
            FindClose(f);
            if (rc != 0)
                return -1;
        }
    }
    return RemoveDirectoryW(w) ? 0 : -1;
}

int plat_remove_tree(const char *path)
{
    wchar_t *w = to_wide(path);
    if (!w)
        return -1;
    int rc = remove_tree_w(w);
    free(w);
    return rc;
}

int plat_random_bytes(void *buf, size_t len)
{
    unsigned char *p = buf;
    while (len > 0) {
        ULONG chunk = len > 0x10000000 ? 0x10000000 : (ULONG)len;
        if (BCryptGenRandom(NULL, p, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
            return -1;
        p += chunk;
        len -= chunk;
    }
    return 0;
}

int plat_open_url(const char *url)
{
    /* ShellExecute runs anything; only hand it web URLs. */
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7))
        return -1;
    wchar_t *w = to_wide(url);
    if (!w)
        return -1;
    HINSTANCE r = ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
    return (INT_PTR)r > 32 ? 0 : -1;
}

int plat_startup(int *argc, char ***argv)
{
    SetConsoleOutputCP(CP_UTF8); /* fails harmlessly without a console */
    int n;
    wchar_t **wv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!wv)
        return -1;
    size_t bytes = (size_t)(n + 1) * sizeof(char *);
    for (int i = 0; i < n; i++) {
        int k = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, NULL, 0, NULL, NULL);
        if (k <= 0) {
            LocalFree(wv);
            return -1;
        }
        bytes += (size_t)k;
    }
    /* One block that lives as long as the process, like the CRT's argv. */
    char **out = malloc(bytes);
    if (!out) {
        LocalFree(wv);
        return -1;
    }
    char *s = (char *)(out + n + 1), *end = (char *)out + bytes;
    for (int i = 0; i < n; i++) {
        out[i] = s;
        s += WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, s, (int)(end - s), NULL, NULL);
    }
    out[n] = NULL;
    LocalFree(wv);
    *argc = n;
    *argv = out;
    return 0;
}

static volatile LONG g_quit;
static SRWLOCK g_quit_lock = SRWLOCK_INIT;
static void (*g_quit_fn)(void *);
static void *g_quit_ud;
static int g_quit_installed;

/* Runs on a thread the system creates, so calling back is safe. After a
 * close/logoff/shutdown event Windows ends the process once this returns;
 * the kill-on-close job still takes the children down with it. */
static BOOL WINAPI quit_ctrl(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_quit, 1);
    AcquireSRWLockExclusive(&g_quit_lock);
    if (g_quit_fn)
        g_quit_fn(g_quit_ud);
    ReleaseSRWLockExclusive(&g_quit_lock);
    return TRUE;
}

int plat_quit_watch(void (*on_quit)(void *ud), void *ud)
{
    AcquireSRWLockExclusive(&g_quit_lock);
    g_quit_fn = on_quit;
    g_quit_ud = ud;
    if (!g_quit_installed)
        g_quit_installed = SetConsoleCtrlHandler(quit_ctrl, TRUE) != 0;
    int ok = g_quit_installed;
    ReleaseSRWLockExclusive(&g_quit_lock);
    return ok ? 0 : -1;
}

int plat_quit_requested(void)
{
    return InterlockedCompareExchange(&g_quit, 0, 0) != 0;
}

uint64_t plat_now_ms(void)
{
    return (uint64_t)GetTickCount64();
}

void plat_sleep_ms(unsigned ms)
{
    Sleep(ms);
}

/* --- threads --- */
struct plat_thread { HANDLE h; void (*fn)(void *); void *arg; };
struct plat_mutex  { SRWLOCK lock; };
struct plat_event  { SRWLOCK lock; CONDITION_VARIABLE cv; int signaled; };

static DWORD WINAPI thread_tramp(LPVOID p)
{
    plat_thread *t = p;
    t->fn(t->arg);
    return 0;
}

plat_thread *plat_thread_start(void (*fn)(void *), void *arg)
{
    plat_thread *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, 0, thread_tramp, t, 0, NULL);
    if (!t->h) {
        free(t);
        return NULL;
    }
    return t;
}

void plat_thread_join(plat_thread *t)
{
    if (!t)
        return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

plat_mutex *plat_mutex_new(void)
{
    plat_mutex *m = calloc(1, sizeof *m);
    if (m)
        InitializeSRWLock(&m->lock);
    return m;
}

void plat_mutex_lock(plat_mutex *m)   { AcquireSRWLockExclusive(&m->lock); }
void plat_mutex_unlock(plat_mutex *m) { ReleaseSRWLockExclusive(&m->lock); }
void plat_mutex_free(plat_mutex *m)   { free(m); }

plat_event *plat_event_new(void)
{
    plat_event *e = calloc(1, sizeof *e);
    if (e) {
        InitializeSRWLock(&e->lock);
        InitializeConditionVariable(&e->cv);
    }
    return e;
}

void plat_event_signal(plat_event *e)
{
    AcquireSRWLockExclusive(&e->lock);
    e->signaled = 1;
    ReleaseSRWLockExclusive(&e->lock);
    WakeConditionVariable(&e->cv);
}

int plat_event_wait(plat_event *e, unsigned timeout_ms)
{
    uint64_t deadline = plat_now_ms() + timeout_ms;
    AcquireSRWLockExclusive(&e->lock);
    while (!e->signaled) {
        uint64_t now = plat_now_ms();
        if (now >= deadline)
            break;
        /* Spurious wakeups loop back with the remaining time. */
        SleepConditionVariableSRW(&e->cv, &e->lock, (DWORD)(deadline - now), 0);
    }
    int got = e->signaled;
    e->signaled = 0;
    ReleaseSRWLockExclusive(&e->lock);
    return got;
}

void plat_event_free(plat_event *e) { free(e); }

/* --- child processes --- */
struct plat_proc {
    HANDLE process, job, out;
    SRWLOCK lock;
    int exited;
};

/* Append one argument with MSVCRT quoting rules. */
static int append_arg(wchar_t *cmd, size_t cap, size_t *pos, const wchar_t *a)
{
#define PUT(ch) do { if (*pos + 1 >= cap) return -1; cmd[(*pos)++] = (ch); } while (0)
    if (*pos)
        PUT(L' ');
    if (*a && !wcspbrk(a, L" \t\"")) {
        for (; *a; a++)
            PUT(*a);
    } else {
        PUT(L'"');
        for (;; a++) {
            size_t bs = 0;
            while (*a == L'\\') { bs++; a++; }
            if (!*a) {
                for (size_t i = 0; i < bs * 2; i++) PUT(L'\\');
                break;
            }
            if (*a == L'"') {
                for (size_t i = 0; i < bs * 2 + 1; i++) PUT(L'\\');
            } else {
                for (size_t i = 0; i < bs; i++) PUT(L'\\');
            }
            PUT(*a);
        }
        PUT(L'"');
    }
    cmd[*pos] = 0;
    return 0;
#undef PUT
}

plat_proc *plat_proc_spawn(const char *const argv[])
{
    enum { CMD_CAP = 32768 };
    wchar_t *cmd = malloc(CMD_CAP * sizeof *cmd), *app = to_wide(argv[0]);
    plat_proc *p = calloc(1, sizeof *p);
    HANDLE rd = NULL, wr = NULL, nul = INVALID_HANDLE_VALUE;
    if (!cmd || !app || !p)
        goto fail;
    size_t pos = 0;
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++) {
        wchar_t *w = to_wide(argv[i]);
        int rc = w ? append_arg(cmd, CMD_CAP, &pos, w) : -1;
        free(w);
        if (rc != 0)
            goto fail;
    }
    SECURITY_ATTRIBUTES inherit = { sizeof inherit, NULL, TRUE };
    if (!CreatePipe(&rd, &wr, &inherit, 0) || !SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0))
        goto fail;
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                      OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE)
        goto fail;
    /* Kill-on-close job: the child dies with us even if we crash. */
    p->job = CreateJobObjectW(NULL, NULL);
    if (p->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(p->job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(app, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        NULL, NULL, &si, &pi))
        goto fail;
    if (p->job)
        AssignProcessToJobObject(p->job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(wr);
    CloseHandle(nul);
    free(cmd);
    free(app);
    p->process = pi.hProcess;
    p->out = rd;
    InitializeSRWLock(&p->lock);
    return p;
fail:
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (p && p->job) CloseHandle(p->job);
    free(cmd);
    free(app);
    free(p);
    return NULL;
}

long plat_proc_read(plat_proc *p, char *buf, size_t cap)
{
    DWORD got = 0;
    if (cap > 0x7fffffff)
        cap = 0x7fffffff;
    if (!ReadFile(p->out, buf, (DWORD)cap, &got, NULL))
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    return (long)got;
}

int plat_proc_wait(plat_proc *p, unsigned timeout_ms)
{
    DWORD code;
    if (!p || WaitForSingleObject(p->process, timeout_ms) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(p->process, &code) || code > 255)
        return -1;
    return (int)code;
}

void plat_proc_terminate(plat_proc *p)
{
    if (!p)
        return;
    AcquireSRWLockExclusive(&p->lock);
    if (!p->exited) {
        /* A windowless console child has no graceful stop signal; force it. */
        TerminateProcess(p->process, 1);
        WaitForSingleObject(p->process, 2000);
        p->exited = 1;
    }
    ReleaseSRWLockExclusive(&p->lock);
}

void plat_proc_kill(plat_proc *p)
{
    if (!p)
        return;
    plat_proc_terminate(p);
    CloseHandle(p->out);
    CloseHandle(p->process);
    if (p->job)
        CloseHandle(p->job);
    free(p);
}

/* System32's tar.exe, never one found through PATH or the current dir. */
static int tar_path(char *out, size_t cap)
{
    wchar_t sys[MAX_PATH + 16];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return -1;
    wmemcpy(sys + n, L"\\tar.exe", 9);
    return from_wide(sys, out, cap);
}
#define TAR_EXTRACT "-xf" /* bsdtar reads .zip too */

/* --- loopback TCP --- */
static INIT_ONCE g_wsa_once = INIT_ONCE_STATIC_INIT;
static int g_wsa_ok;

static BOOL CALLBACK wsa_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    WSADATA d;
    g_wsa_ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    return TRUE;
}

static int wait_sock(plat_sock s, short events, unsigned timeout_ms)
{
    WSAPOLLFD pfd;
    pfd.fd = (SOCKET)s;
    pfd.events = events;
    pfd.revents = 0;
    return WSAPoll(&pfd, 1, (INT)timeout_ms) > 0;
}

plat_sock plat_listen_loopback(int port)
{
    InitOnceExecuteOnce(&g_wsa_once, wsa_init, NULL, NULL);
    if (!g_wsa_ok)
        return PLAT_SOCK_INVALID;
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET)
        return PLAT_SOCK_INVALID;
    BOOL one = TRUE; /* nobody else may bind the redirect port while we use it */
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 4) != 0) {
        closesocket(s);
        return PLAT_SOCK_INVALID;
    }
    return (plat_sock)s;
}

plat_sock plat_accept(plat_sock s, unsigned timeout_ms)
{
    if (!wait_sock(s, POLLRDNORM, timeout_ms))
        return PLAT_SOCK_INVALID;
    SOCKET c = accept((SOCKET)s, NULL, NULL);
    if (c == INVALID_SOCKET)
        return PLAT_SOCK_INVALID;
    SetHandleInformation((HANDLE)c, HANDLE_FLAG_INHERIT, 0);
    return (plat_sock)c;
}

long plat_recv(plat_sock s, char *buf, size_t cap, unsigned timeout_ms)
{
    if (!wait_sock(s, POLLRDNORM, timeout_ms))
        return -1;
    int n = recv((SOCKET)s, buf, cap > 0x7fffffff ? 0x7fffffff : (int)cap, 0);
    return n < 0 ? -1 : (long)n;
}

long plat_send(plat_sock s, const char *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done > 0x7fffffff ? 0x7fffffff : len - done;
        int n = send((SOCKET)s, buf + done, (int)chunk, 0);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return (long)done;
}

void plat_close(plat_sock s)
{
    if (s != PLAT_SOCK_INVALID)
        closesocket((SOCKET)s);
}

#else
/* ======================================================================= */
/* POSIX (macOS, Linux)                                                    */
/* ======================================================================= */
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/random.h>
#endif

extern char **environ;

static char g_exe_dir[PLAT_PATH_MAX];
static pthread_once_t g_exe_once = PTHREAD_ONCE_INIT;

static void exe_dir_init(void)
{
    char real[PATH_MAX];
#ifdef __APPLE__
    char raw[PLAT_PATH_MAX];
    uint32_t size = sizeof raw;
    if (_NSGetExecutablePath(raw, &size) != 0 || !realpath(raw, real))
        return;
#else
    if (!realpath("/proc/self/exe", real))
        return;
#endif
    strip_last_component(real);
    if (!real[0]) /* executable in "/" */
        snprintf(real, sizeof real, "/");
    int k = snprintf(g_exe_dir, sizeof g_exe_dir, "%s", real);
    if (k < 0 || (size_t)k >= sizeof g_exe_dir)
        g_exe_dir[0] = '\0'; /* a cut-off path names the wrong folder: fail */
}

const char *plat_exe_dir(void)
{
    pthread_once(&g_exe_once, exe_dir_init);
    return g_exe_dir[0] ? g_exe_dir : NULL;
}

static int set_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFD);
    return (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0) ? -1 : 0;
}

int plat_startup(int *argc, char ***argv)
{
    (void)argc; (void)argv; /* already UTF-8 */
    return signal(SIGPIPE, SIG_IGN) == SIG_ERR ? -1 : 0;
}

static volatile sig_atomic_t g_quit;
static int g_quit_pipe[2] = { -1, -1 };
static pthread_mutex_t g_quit_lock = PTHREAD_MUTEX_INITIALIZER; /* guards fn, ud */
static void (*g_quit_fn)(void *);
static void *g_quit_ud;

/* Self-pipe: write() is async-signal-safe, waking a UI is not. The write
 * end is non-blocking, and a full pipe already holds a pending wake-up. */
static void quit_signal(int sig)
{
    (void)sig;
    int saved = errno;
    g_quit = 1;
    ssize_t r = write(g_quit_pipe[1], "q", 1);
    (void)r;
    errno = saved;
}

/* Parked in read() (no CPU) until a quit signal arrives. */
static void *quit_thread(void *arg)
{
    (void)arg;
    char c;
    for (;;) {
        ssize_t n = read(g_quit_pipe[0], &c, 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return NULL;
        pthread_mutex_lock(&g_quit_lock);
        if (g_quit_fn)
            g_quit_fn(g_quit_ud);
        pthread_mutex_unlock(&g_quit_lock);
    }
}

/* Caller holds g_quit_lock. */
static int quit_install(void)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pthread_t t;
    if (set_cloexec(fds[0]) != 0 || set_cloexec(fds[1]) != 0 ||
        fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    g_quit_pipe[0] = fds[0];
    g_quit_pipe[1] = fds[1];
    if (pthread_create(&t, NULL, quit_thread, NULL) != 0) {
        close(fds[0]);
        close(fds[1]);
        g_quit_pipe[0] = g_quit_pipe[1] = -1;
        return -1;
    }
    pthread_detach(t);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = quit_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* don't fail other threads' syscalls with EINTR */
    static const int sigs[] = { SIGINT, SIGTERM, SIGHUP };
    int rc = 0;
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        struct sigaction old;
        /* Respect an inherited "ignore" (nohup, background shell jobs). */
        if (sigaction(sigs[i], NULL, &old) == 0 && old.sa_handler == SIG_IGN)
            continue;
        if (sigaction(sigs[i], &sa, NULL) != 0)
            rc = -1;
    }
    return rc;
}

int plat_quit_watch(void (*on_quit)(void *ud), void *ud)
{
    pthread_mutex_lock(&g_quit_lock);
    g_quit_fn = on_quit;
    g_quit_ud = ud;
    int rc = g_quit_pipe[0] < 0 ? quit_install() : 0;
    pthread_mutex_unlock(&g_quit_lock);
    return rc;
}

int plat_quit_requested(void)
{
    return g_quit != 0;
}

int plat_mkdir(const char *path)
{
    if (mkdir(path, 0700) == 0)
        return 0;
    struct stat st;
    return (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

static int write_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int plat_write_file_atomic(const char *path, const void *data, size_t len)
{
    char tmp[PLAT_PATH_MAX];
    int n = snprintf(tmp, sizeof tmp, "%s.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof tmp)
        return -1;
    int fd = mkstemp(tmp); /* creates with 0600 */
    if (fd < 0)
        return -1;
    int ok = set_cloexec(fd) == 0 && fchmod(fd, 0600) == 0 &&
             write_all(fd, data, len) == 0 && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = 0;
    if (ok && rename(tmp, path) == 0)
        return 0;
    unlink(tmp);
    return -1;
}

char *plat_read_file(const char *path, size_t max_len, size_t *out_len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    char *buf = NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (unsigned long long)st.st_size > max_len)
        goto out;
    size_t size = (size_t)st.st_size, got = 0;
    buf = malloc(size + 1);
    if (!buf)
        goto out;
    while (got < size) {
        ssize_t r = read(fd, buf + got, size - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break; /* shrank underneath us: return what is there */
        got += (size_t)r;
    }
    buf[got] = '\0';
    if (out_len)
        *out_len = got;
out:
    close(fd);
    return buf;
}

int plat_remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0 ? 0 : -1;
    DIR *d = opendir(path);
    if (!d)
        return -1;
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char child[PLAT_PATH_MAX];
        if (plat_path_join(child, sizeof child, path, e->d_name) != 0 ||
            plat_remove_tree(child) != 0)
            rc = -1;
    }
    closedir(d);
    if (rc != 0)
        return -1;
    return rmdir(path) == 0 ? 0 : -1;
}

int plat_random_bytes(void *buf, size_t len)
{
#ifdef __APPLE__
    arc4random_buf(buf, len);
    return 0;
#else
    unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = getrandom(p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
#endif
}

uint64_t plat_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void plat_sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* --- threads --- */
struct plat_thread { pthread_t tid; void (*fn)(void *); void *arg; };
struct plat_mutex  { pthread_mutex_t m; };
struct plat_event  { pthread_mutex_t m; pthread_cond_t c; int signaled; };

static void *thread_tramp(void *p)
{
    plat_thread *t = p;
    t->fn(t->arg);
    return NULL;
}

plat_thread *plat_thread_start(void (*fn)(void *), void *arg)
{
    plat_thread *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->tid, NULL, thread_tramp, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

void plat_thread_join(plat_thread *t)
{
    if (!t)
        return;
    pthread_join(t->tid, NULL);
    free(t);
}

plat_mutex *plat_mutex_new(void)
{
    plat_mutex *m = calloc(1, sizeof *m);
    if (m && pthread_mutex_init(&m->m, NULL) != 0) {
        free(m);
        return NULL;
    }
    return m;
}

void plat_mutex_lock(plat_mutex *m)   { pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(plat_mutex *m) { pthread_mutex_unlock(&m->m); }

void plat_mutex_free(plat_mutex *m)
{
    if (!m)
        return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

plat_event *plat_event_new(void)
{
    plat_event *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    if (pthread_mutex_init(&e->m, NULL) != 0) {
        free(e);
        return NULL;
    }
    int rc;
#ifdef __APPLE__
    rc = pthread_cond_init(&e->c, NULL); /* waits use the relative variant */
#else
    pthread_condattr_t ca;
    rc = pthread_condattr_init(&ca);
    if (rc == 0) {
        /* Monotonic so wall-clock jumps don't stretch or cut waits. */
        rc = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        if (rc == 0)
            rc = pthread_cond_init(&e->c, &ca);
        pthread_condattr_destroy(&ca);
    }
#endif
    if (rc != 0) {
        pthread_mutex_destroy(&e->m);
        free(e);
        return NULL;
    }
    return e;
}

void plat_event_signal(plat_event *e)
{
    pthread_mutex_lock(&e->m);
    e->signaled = 1;
    pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
}

int plat_event_wait(plat_event *e, unsigned timeout_ms)
{
    uint64_t deadline = plat_now_ms() + timeout_ms;
    pthread_mutex_lock(&e->m);
    while (!e->signaled) {
        uint64_t now = plat_now_ms();
        if (now >= deadline)
            break;
        uint64_t left = deadline - now;
#ifdef __APPLE__
        struct timespec rel = { (time_t)(left / 1000), (long)(left % 1000) * 1000000L };
        pthread_cond_timedwait_relative_np(&e->c, &e->m, &rel);
#else
        struct timespec abs;
        clock_gettime(CLOCK_MONOTONIC, &abs);
        abs.tv_sec += (time_t)(left / 1000);
        abs.tv_nsec += (long)(left % 1000) * 1000000L;
        if (abs.tv_nsec >= 1000000000L) {
            abs.tv_sec++;
            abs.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&e->c, &e->m, &abs);
#endif
    }
    int got = e->signaled;
    e->signaled = 0;
    pthread_mutex_unlock(&e->m);
    return got;
}

void plat_event_free(plat_event *e)
{
    if (!e)
        return;
    pthread_cond_destroy(&e->c);
    pthread_mutex_destroy(&e->m);
    free(e);
}

/* --- child processes --- */
struct plat_proc {
    pid_t pid;
    int out;
    int reaped;
    int code;       /* exit code once reaped, -1 if signaled/unknown */
    pid_t guard;    /* watchdog process, 0 if none */
    int guard_fd;   /* our end of the watchdog's pipe, -1 if none */
    pthread_mutex_t lock; /* serializes waitpid between alive/terminate */
};

/* Spawn with stdin = in_fd (or /dev/null when in_fd < 0) and stdout+stderr
 * = out_fd (or /dev/null when out_fd < 0). Signals the child must see as
 * default are reset because ignored dispositions survive exec. */
static int spawn_child(pid_t *pid, const char *const argv[], int search_path, int in_fd, int out_fd)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    int rc;
    if ((rc = posix_spawn_file_actions_init(&fa)) != 0)
        return rc;
    if ((rc = posix_spawnattr_init(&at)) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        return rc;
    }
    sigset_t none, defs;
    sigemptyset(&none);
    sigemptyset(&defs);
    sigaddset(&defs, SIGPIPE);
    sigaddset(&defs, SIGINT);
    sigaddset(&defs, SIGTERM);
    sigaddset(&defs, SIGHUP);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT; /* macOS: inherit only fds 0-2 */
#endif
    rc = posix_spawnattr_setflags(&at, flags);
    if (!rc) rc = posix_spawnattr_setsigmask(&at, &none);
    if (!rc) rc = posix_spawnattr_setsigdefault(&at, &defs);
    if (in_fd >= 0) {
        if (!rc) rc = posix_spawn_file_actions_adddup2(&fa, in_fd, 0);
    } else {
        if (!rc) rc = posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    }
    if (out_fd >= 0) {
        if (!rc) rc = posix_spawn_file_actions_adddup2(&fa, out_fd, 1);
        if (!rc) rc = posix_spawn_file_actions_adddup2(&fa, out_fd, 2);
    } else {
        if (!rc) rc = posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        if (!rc) rc = posix_spawn_file_actions_adddup2(&fa, 1, 2);
    }
    if (!rc) {
        char *const *av = (char *const *)(uintptr_t)argv; /* POSIX signature lacks const */
        rc = search_path ? posix_spawnp(pid, argv[0], &fa, &at, av, environ)
                         : posix_spawn(pid, argv[0], &fa, &at, av, environ);
    }
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc;
}

#ifdef __linux__
/* posix_spawn can't run code in the child, so fork: PR_SET_PDEATHSIG makes
 * the kernel SIGTERM the child when the thread that forked it dies, crash
 * and SIGKILL included. Only async-signal-safe calls between fork and exec. */
static int spawn_tied(pid_t *pid, const char *const argv[], int out_fd)
{
    if (access(argv[0], X_OK) != 0)
        return -1; /* fail here like posix_spawn would, not in the child */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigset_t none;
    sigemptyset(&none);
    pid_t parent = getpid();
    pid_t c = fork();
    if (c < 0)
        return -1;
    if (c == 0) {
        static const int defs[] = { SIGPIPE, SIGINT, SIGTERM, SIGHUP };
        for (size_t i = 0; i < sizeof defs / sizeof defs[0]; i++)
            sigaction(defs[i], &dfl, NULL);
        sigprocmask(SIG_SETMASK, &none, NULL);
        int nul = open("/dev/null", O_RDONLY);
        if (nul < 0 || (nul != 0 && (dup2(nul, 0) < 0 || close(nul) != 0)) ||
            dup2(out_fd, 1) < 0 || dup2(out_fd, 2) < 0 ||
            prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent)
            _exit(127); /* the last check: we already died before prctl */
        execv(argv[0], (char *const *)(uintptr_t)argv);
        _exit(127);
    }
    *pid = c;
    return 0;
}
#else
/* No PR_SET_PDEATHSIG here (macOS): a /bin/sh watchdog blocks reading a
 * pipe whose only write end we hold (close-on-exec, so the child never has
 * it). However we die, the kernel closes it, read sees EOF and the watchdog
 * sends the child SIGTERM. One idle process is the price; a pid file would
 * leave a crashed session's engine playing until the next start. Not
 * fatal if it fails: the quit signals are still handled. */
static void start_guard(plat_proc *p)
{
    char script[64];
    int fds[2];
    int n = snprintf(script, sizeof script, "read x; kill %ld", (long)p->pid);
    if (n < 0 || (size_t)n >= sizeof script || pipe(fds) != 0)
        return;
    const char *argv[] = { "/bin/sh", "-c", script, NULL };
    if (set_cloexec(fds[0]) == 0 && set_cloexec(fds[1]) == 0 &&
        spawn_child(&p->guard, argv, 0, fds[0], -1) == 0) {
        close(fds[0]);
        p->guard_fd = fds[1];
        return;
    }
    p->guard = 0;
    close(fds[0]);
    close(fds[1]);
}
#endif

/* Caller holds p->lock. Ends the watchdog before the child's pid is freed
 * (or right after a natural exit), so it can never signal a reused pid. */
static void stop_guard(plat_proc *p)
{
    if (p->guard > 0) {
        kill(p->guard, SIGKILL);
        while (waitpid(p->guard, NULL, 0) < 0 && errno == EINTR) {
        }
        p->guard = 0;
    }
    if (p->guard_fd >= 0) {
        close(p->guard_fd);
        p->guard_fd = -1;
    }
}

plat_proc *plat_proc_spawn(const char *const argv[])
{
    plat_proc *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->guard_fd = -1;
    int fds[2];
#ifdef __linux__
    if (pipe2(fds, O_CLOEXEC) != 0) {
#else
    if (pipe(fds) != 0) {
#endif
        free(p);
        return NULL;
    }
#ifndef __linux__
    if (set_cloexec(fds[0]) != 0 || set_cloexec(fds[1]) != 0)
        goto fail;
#endif
    if (pthread_mutex_init(&p->lock, NULL) != 0)
        goto fail;
#ifdef __linux__
    if (spawn_tied(&p->pid, argv, fds[1]) != 0) {
#else
    if (spawn_child(&p->pid, argv, 0, -1, fds[1]) != 0) {
#endif
        pthread_mutex_destroy(&p->lock);
        goto fail;
    }
#ifndef __linux__
    start_guard(p);
#endif
    close(fds[1]); /* EOF reaches the reader once the child exits */
    p->out = fds[0];
    return p;
fail:
    close(fds[0]);
    close(fds[1]);
    free(p);
    return NULL;
}

long plat_proc_read(plat_proc *p, char *buf, size_t cap)
{
    for (;;) {
        ssize_t n = read(p->out, buf, cap);
        if (n < 0 && errno == EINTR)
            continue;
        return n < 0 ? -1 : (long)n;
    }
}

/* Caller holds p->lock. Returns 1 once the child has been reaped. */
static int reap_nohang(plat_proc *p)
{
    if (!p->reaped) {
        int st = 0;
        pid_t r = waitpid(p->pid, &st, WNOHANG);
        if (r == p->pid || (r < 0 && errno == ECHILD)) {
            p->reaped = 1;
            p->code = r == p->pid && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            stop_guard(p);
        }
    }
    return p->reaped;
}

/* Caller holds p->lock. POSIX has no timed waitpid; back off from 1 ms to
 * 64 ms so the usual quick exit is reaped fast with only a few dozen
 * wakeups. Returns 1 once reaped. */
static int reap_within(plat_proc *p, unsigned timeout_ms)
{
    uint64_t deadline = plat_now_ms() + timeout_ms;
    unsigned step = 1;
    while (!reap_nohang(p) && plat_now_ms() < deadline) {
        plat_sleep_ms(step);
        if (step < 64)
            step *= 2;
    }
    return p->reaped;
}

int plat_proc_wait(plat_proc *p, unsigned timeout_ms)
{
    if (!p)
        return -1;
    pthread_mutex_lock(&p->lock);
    int code = reap_within(p, timeout_ms) ? p->code : -1;
    pthread_mutex_unlock(&p->lock);
    return code;
}

void plat_proc_terminate(plat_proc *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->lock);
    if (!reap_nohang(p)) {
        stop_guard(p);
        kill(p->pid, SIGTERM);
        if (!reap_within(p, 2000)) {
            kill(p->pid, SIGKILL);
            while (waitpid(p->pid, NULL, 0) < 0 && errno == EINTR) {
            }
            p->reaped = 1;
            p->code = -1;
        }
    }
    pthread_mutex_unlock(&p->lock);
}

void plat_proc_kill(plat_proc *p)
{
    if (!p)
        return;
    plat_proc_terminate(p);
    close(p->out);
    pthread_mutex_destroy(&p->lock);
    free(p);
}

static int tar_path(char *out, size_t cap)
{
    /* Absolute path: plat_proc_spawn doesn't search PATH. Older non-merged
     * /usr Linux systems only have /bin/tar. */
    const char *tar = access("/usr/bin/tar", X_OK) == 0 ? "/usr/bin/tar" : "/bin/tar";
    int n = snprintf(out, cap, "%s", tar);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}
#define TAR_EXTRACT "-xzf"

/* Reaps the opener so it never lingers as a zombie. */
static void *reap_thread(void *arg)
{
    pid_t pid = (pid_t)(intptr_t)arg;
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
    return NULL;
}

int plat_open_url(const char *url)
{
    /* Only web URLs: the opener would happily launch local files/apps. */
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7))
        return -1;
#ifdef __APPLE__
    const char *argv[] = { "/usr/bin/open", url, NULL };
    int search = 0;
#else
    const char *argv[] = { "xdg-open", url, NULL };
    int search = 1;
#endif
    pid_t pid;
    if (spawn_child(&pid, argv, search, -1, -1) != 0)
        return -1;
    pthread_t t;
    if (pthread_create(&t, NULL, reap_thread, (void *)(intptr_t)pid) == 0)
        pthread_detach(t);
    else
        waitpid(pid, NULL, 0);
    return 0;
}

/* --- loopback TCP --- */
static int wait_fd(int fd, short events, unsigned timeout_ms)
{
    struct pollfd pfd = { fd, events, 0 };
    uint64_t deadline = plat_now_ms() + timeout_ms;
    for (;;) {
        uint64_t now = plat_now_ms();
        int left = now >= deadline ? 0 : (int)(deadline - now);
        int r = poll(&pfd, 1, left);
        if (r < 0 && errno == EINTR)
            continue;
        return r > 0;
    }
}

static void sock_setup(int fd)
{
    set_cloexec(fd);
#ifdef SO_NOSIGPIPE
    int one = 1; /* a peer closing early must not kill us with SIGPIPE */
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

plat_sock plat_listen_loopback(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return PLAT_SOCK_INVALID;
    sock_setup(s);
    int one = 1; /* rebind right after a previous login left TIME_WAIT */
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 4) != 0) {
        close(s);
        return PLAT_SOCK_INVALID;
    }
    return (plat_sock)s;
}

plat_sock plat_accept(plat_sock s, unsigned timeout_ms)
{
    if (!wait_fd((int)s, POLLIN, timeout_ms))
        return PLAT_SOCK_INVALID;
    int c;
    do
        c = accept((int)s, NULL, NULL);
    while (c < 0 && errno == EINTR);
    if (c < 0)
        return PLAT_SOCK_INVALID;
    sock_setup(c);
    return (plat_sock)c;
}

long plat_recv(plat_sock s, char *buf, size_t cap, unsigned timeout_ms)
{
    if (!wait_fd((int)s, POLLIN, timeout_ms))
        return -1;
    ssize_t n;
    do
        n = recv((int)s, buf, cap, 0);
    while (n < 0 && errno == EINTR);
    return n < 0 ? -1 : (long)n;
}

long plat_send(plat_sock s, const char *buf, size_t len)
{
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    size_t done = 0;
    while (done < len) {
        ssize_t n = send((int)s, buf + done, len - done, flags);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return (long)done;
}

void plat_close(plat_sock s)
{
    if (s != PLAT_SOCK_INVALID)
        close((int)s);
}

#endif

/* --- archive extraction (both OSes, on top of plat_proc) --- */
int plat_untar(const char *archive, const char *dir)
{
    char tar[PLAT_PATH_MAX];
    if (tar_path(tar, sizeof tar) != 0)
        return -1;
    const char *argv[] = { tar, TAR_EXTRACT, archive, "-C", dir, NULL };
    plat_proc *p = plat_proc_spawn(argv);
    if (!p)
        return -1;
    char buf[512];
    while (plat_proc_read(p, buf, sizeof buf) > 0) {
    } /* drain: a full pipe would stall tar */
    int code = plat_proc_wait(p, 60000);
    plat_proc_kill(p);
    return code == 0 ? 0 : -1;
}
