/* Settings written back (settings.h): ini keys and one-line files changed in place, and the
 * game started again for the settings that count only at its start. (Not an HLE of anything:
 * named hle_ so both builds' runtime file patterns take it.)
 *
 * A file is read whole, changed line by line and written to <file>.tmp, which then replaces it,
 * so a crash halfway leaves the old file. Its line ends (CRLF or LF) are kept. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <signal.h>
#include <spawn.h>
extern char **environ;
const char *rt_exe_path(void);   /* (macos/platform.c: the program, from _NSGetExecutablePath) */
#endif

#include "host.h"
#include "settings.h"

static SRWLOCK lock = SRWLOCK_INIT;
static int online_files;
#ifdef __APPLE__
static char **start_argv;   /* (settings_restart: macOS has no /proc/self/cmdline) */
#endif

void settings_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--online")) online_files = 1;
#ifdef __APPLE__
    start_argv = calloc((size_t)argc + 1, sizeof *start_argv);
    for (int i = 0; start_argv && i < argc; i++) start_argv[i] = _strdup(argv[i]);
#endif
}

int settings_online(void) { return online_files; }

/* ---- a file as lines --------------------------------------------------------------------- */
typedef struct Text { char **line; int n, cap, crlf; } Text;

static void text_add(Text *t, int at, const char *s, size_t len) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->line = realloc(t->line, (size_t)t->cap * sizeof *t->line);
    }
    memmove(t->line + at + 1, t->line + at, (size_t)(t->n - at) * sizeof *t->line);
    t->line[at] = malloc(len + 1);
    memcpy(t->line[at], s, len);
    t->line[at][len] = 0;
    t->n++;
}

static void text_remove(Text *t, int at) {
    free(t->line[at]);
    memmove(t->line + at, t->line + at + 1, (size_t)(t->n - at - 1) * sizeof *t->line);
    t->n--;
}

static void text_free(Text *t) {
    for (int i = 0; i < t->n; i++) free(t->line[i]);
    free(t->line);
    memset(t, 0, sizeof *t);
}

/* 0 when the file is missing (an empty text, with this system's line ends). */
static int text_load(const char *path, Text *t) {
    memset(t, 0, sizeof *t);
#ifdef _WIN32
    t->crlf = 1;
#endif
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)(size > 0 ? size : 0) + 1);
    size_t got = fread(buf, 1, (size_t)(size > 0 ? size : 0), f);
    fclose(f);
    buf[got] = 0;
    char *p = buf, *end = buf + got;
    if (got >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;   /* (Notepad's BOM) */
    int seen_eol = 0;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *e = nl ? nl : end;
        size_t len = (size_t)(e - p);
        if (nl && !seen_eol) { seen_eol = 1; t->crlf = len && e[-1] == '\r'; }
        if (len && p[len - 1] == '\r') len--;
        text_add(t, t->n, p, len);
        p = nl ? nl + 1 : end;
    }
    free(buf);
    return 1;
}

static int text_save(const char *path, const Text *t) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "[settings] cannot write %s\n", tmp); return 0; }
    int ok = 1;
    for (int i = 0; i < t->n && ok; i++)
        ok = fputs(t->line[i], f) >= 0 && fputs(t->crlf ? "\r\n" : "\n", f) >= 0;
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) { fprintf(stderr, "[settings] cannot replace %s\n", path); remove(tmp); }
    return ok;
}

/* ---- ini lines ------------------------------------------------------------------------- */
static const char *skip_space(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

static int blank(const char *s) { return !*skip_space(s); }

static int comment(const char *s) { s = skip_space(s); return *s == ';' || *s == '#'; }

/* [name] -> name with its spaces made single (so "[device  ps5]" is "device ps5"); 0 if not a header. */
static int header_name(const char *s, char *out, int size) {
    s = skip_space(s);
    if (*s != '[') return 0;
    const char *e = strchr(s, ']');
    if (!e) return 0;
    int n = 0, space = 0;
    for (s = skip_space(s + 1); s < e && n < size - 1; s++) {
        if (*s == ' ' || *s == '\t') { space = 1; continue; }
        if (space && n) out[n++] = ' ';
        space = 0;
        out[n++] = *s;
    }
    out[n] = 0;
    return 1;
}

static int same_section(const char *a, const char *b) {
    char x[160], y[160];
    snprintf(y, sizeof y, "[%s]", b);
    return header_name(y, x, sizeof x) && !_stricmp(a, x);
}

/* The key of a key = value line (into out), or 0. */
static int line_key(const char *s, char *out, int size) {
    s = skip_space(s);
    if (*s == ';' || *s == '#' || *s == '[') return 0;
    const char *eq = strchr(s, '=');
    if (!eq) return 0;
    const char *e = eq;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    int n = (int)(e - s) < size - 1 ? (int)(e - s) : size - 1;
    memcpy(out, s, (size_t)n);
    out[n] = 0;
    return n > 0;
}

/* Where a value's own comment starts (';' or '#' after a space), or its end. */
static const char *comment_after(const char *value) {
    for (const char *p = value; *p; p++)
        if ((*p == ';' || *p == '#') && p > value && (p[-1] == ' ' || p[-1] == '\t')) return p;
    return value + strlen(value);
}

/* The line of the key in the section (flat: anywhere), or -1; *last = the section's last
 * non-blank line (-1: no such section). */
static int find_key(const Text *t, const char *section, const char *key, int *last) {
    int in = section == NULL, found = -1;
    *last = section ? -1 : t->n - 1;
    for (int i = 0; i < t->n; i++) {
        char name[160], k[96];
        if (section && header_name(t->line[i], name, sizeof name)) {
            in = same_section(name, section);
            if (in) *last = i;
            continue;
        }
        if (!in) continue;
        if (section && !blank(t->line[i])) *last = i;
        if (found < 0 && line_key(t->line[i], k, sizeof k) && !_stricmp(k, key)) found = i;
    }
    return found;
}

int settings_ini_get(const char *path, const char *section, const char *key, char *out, int size) {
    Text t;
    AcquireSRWLockShared(&lock);
    text_load(path, &t);
    ReleaseSRWLockShared(&lock);
    int last, i = find_key(&t, section, key, &last);
    if (i >= 0) {
        const char *v = skip_space(strchr(t.line[i], '=') + 1), *e = comment_after(v);
        while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
        int n = (int)(e - v) < size - 1 ? (int)(e - v) : size - 1;
        memcpy(out, v, (size_t)n);
        out[n] = 0;
    }
    text_free(&t);
    return i >= 0;
}

int settings_ini_set(const char *path, const char *section, const char *key, const char *value) {
    AcquireSRWLockExclusive(&lock);
    Text t;
    text_load(path, &t);
    int last, i = find_key(&t, section, key, &last), ok = 1, changed = 1;
    char buf[1024];
    if (i >= 0 && !value) text_remove(&t, i);
    else if (i >= 0) {
        const char *line = t.line[i], *eq = strchr(line, '='), *v = skip_space(eq + 1), *c = comment_after(v), *ws = c;
        while (ws > v && (ws[-1] == ' ' || ws[-1] == '\t')) ws--;
        if (!*c) ws = c;   /* (no comment: trailing spaces go) */
        snprintf(buf, sizeof buf, "%.*s %s%s", (int)(eq - line + 1), line, value, ws);
        changed = strcmp(buf, line) != 0;
        if (changed) { free(t.line[i]); t.line[i] = _strdup(buf); }
    } else if (value) {
        snprintf(buf, sizeof buf, "%s = %s", key, value);
        if (last >= 0) text_add(&t, last + 1, buf, strlen(buf));
        else {
            if (t.n && !blank(t.line[t.n - 1])) text_add(&t, t.n, "", 0);
            if (section) {
                char head[160];
                snprintf(head, sizeof head, "[%s]", section);
                text_add(&t, t.n, head, strlen(head));
            }
            text_add(&t, t.n, buf, strlen(buf));
        }
    } else changed = 0;
    if (changed) ok = text_save(path, &t);
    ReleaseSRWLockExclusive(&lock);
    text_free(&t);
    if (changed && ok) fprintf(stderr, "[settings] %s: %s%s%s%s = %s\n", path, section ? "[" : "", section ? section : "", section ? "] " : "", key,
                               value ? value : "(removed)");
    return ok;
}

/* ---- one-line files ------------------------------------------------------------------- */
static int first_line(const Text *t) {
    for (int i = 0; i < t->n; i++) {
        const char *s = skip_space(t->line[i]);
        if (*s && *s != '#') return i;
    }
    return -1;
}

int settings_line_read(const char *path, char *out, int size) {
    Text t;
    AcquireSRWLockShared(&lock);
    text_load(path, &t);
    ReleaseSRWLockShared(&lock);
    int i = first_line(&t);
    if (i >= 0) {
        const char *s = skip_space(t.line[i]), *e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        int n = (int)(e - s) < size - 1 ? (int)(e - s) : size - 1;
        memcpy(out, s, (size_t)n);
        out[n] = 0;
    } else if (size > 0) out[0] = 0;
    text_free(&t);
    return i >= 0;
}

int settings_line_write(const char *path, const char *line) {
    AcquireSRWLockExclusive(&lock);
    Text t;
    text_load(path, &t);
    int i = first_line(&t), ok = 1;
    if (i >= 0 && !strcmp(t.line[i], line)) ok = -1;   /* (as it is) */
    else if (i >= 0) { free(t.line[i]); t.line[i] = _strdup(line); }
    else text_add(&t, t.n, line, strlen(line));
    if (ok > 0) ok = text_save(path, &t);
    ReleaseSRWLockExclusive(&lock);
    text_free(&t);
    if (ok > 0) fprintf(stderr, "[settings] %s: %s\n", path, line);
    return ok != 0;
}

/* ---- starting again --------------------------------------------------------------------- */
static const char *const NET_VARS[] = {"SSBU_NET_RELAY", "SSBU_NET_ROOM", "SSBU_NET_NAME", "SSBU_NET_DIRECT"};

#ifdef _WIN32
void settings_restart(void) {
    if (online_files)
        for (u32 i = 0; i < sizeof NET_VARS / sizeof *NET_VARS; i++) SetEnvironmentVariableA(NET_VARS[i], NULL);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;   /* the same log (cmd's redirection of the first start) */
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE std[3] = {si.hStdInput, si.hStdOutput, si.hStdError};
    for (int i = 0; i < 3; i++)
        if (std[i] && std[i] != INVALID_HANDLE_VALUE) SetHandleInformation(std[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    wchar_t *cmd = _wcsdup(GetCommandLineW());
    PROCESS_INFORMATION pi;
    fprintf(stderr, "[settings] starting the game again\n");
    fflush(stderr);
    fflush(stdout);
    if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[settings] could not start the game again (error %lu)\n", GetLastError());
        free(cmd);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
}
#elif defined __APPLE__
/* The program in place of this one (the same process, so the launcher's `open -W` keeps waiting
 * for it), with the arguments it started with. The log stays stdout and stderr (without SSBU_LOG,
 * which would start it again empty); every other file and socket is closed. */
void settings_restart(void) {
    if (online_files)
        for (u32 i = 0; i < sizeof NET_VARS / sizeof *NET_VARS; i++) unsetenv(NET_VARS[i]);
    const char *exe = rt_exe_path();
    if (!start_argv || !start_argv[0] || !exe[0]) { fprintf(stderr, "[settings] could not start the game again (no command line)\n"); return; }
    unsetenv("SSBU_LOG");
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t files;
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC | POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK);
    posix_spawnattr_setsigmask(&attr, &none);   /* (this thread's blocked signals would be the new main thread's) */
    posix_spawn_file_actions_init(&files);
    for (int fd = 0; fd < 3; fd++) posix_spawn_file_actions_addinherit_np(&files, fd);
    fprintf(stderr, "[settings] starting the game again\n");
    fflush(stderr);
    fflush(stdout);
    int err = posix_spawn(NULL, exe, &files, &attr, start_argv, environ);   /* (returns only when it failed) */
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    fprintf(stderr, "[settings] could not start the game again (error %d)\n", err);
}
#else
void settings_restart(void) {
    if (online_files)
        for (u32 i = 0; i < sizeof NET_VARS / sizeof *NET_VARS; i++) unsetenv(NET_VARS[i]);
    static char buf[16384];
    char *argv[256];
    int argc = 0;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    for (size_t p = 0; p < n && argc < 255; p += strlen(buf + p) + 1) argv[argc++] = buf + p;
    argv[argc] = NULL;
    if (!argc) { fprintf(stderr, "[settings] could not start the game again (no command line)\n"); return; }
    fprintf(stderr, "[settings] starting the game again\n");
    fflush(stderr);
    fflush(stdout);
    execv("/proc/self/exe", argv);
    fprintf(stderr, "[settings] could not start the game again\n");
}
#endif
