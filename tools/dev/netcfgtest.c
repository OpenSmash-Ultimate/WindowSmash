/* Offline checks of where the game goes online (runtime/net_relay.c's configure): the lobby from
 * SSBU_NET_RELAY, or from online_server.txt when started with --online as Play SSBU.bat does,
 * and net_relay_address(), which the game's Online mode (hle_online.c) logs in through. Nothing
 * is sent: only the settings are read. Each case runs in a child process of its own (the settings
 * are read once a process), in a folder of its own.
 *
 *   python -c "import sys; sys.path.insert(0,'tools'); import cc, subprocess; subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.CFLAGS, 'tools/dev/netcfgtest.c', '/Febuild/netcfgtest.exe'], check=True)"
 *   build\netcfgtest.exe
 *
 * (tests/test_settings.py builds and runs it.) Exit code 0 when every check passes. */
#include "../../runtime/net_relay.c"
#include "../../runtime/hle_settings.c"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

/* (what net_relay.c reaches outside itself; never called here) */
const char *rt_data_dir = ".";
const RtModule rt_module_main;
void vsock_deliver(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len) {}
int ldn_in_network(u32 ip) { return 0; }
void ldn_listing(u32 seq, u32 owner, const u8 *info) {}
void ldn_list_end(u32 seq) {}

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct Case {
    const char *name, *server_file, *env, *args;   /* server_file NULL: none; env: NAME=value or NULL */
    const char *address, *room;                     /* what net_relay_address() and the room are; address NULL: none */
} Case;

static const Case CASES[] = {
    {"public lobby from the file", "# the lobby\nsmash.recomps.wtf:39421\n", NULL, "--online", "smash.recomps.wtf:39421", "smash"},
    {"lobby code from the file", "smash.recomps.wtf:39421 OurCode\n", NULL, "--online", "smash.recomps.wtf:39421", "OurCode"},
    {"offline in the file", "off\n", NULL, "--online", NULL, "smash"},
    {"no file", NULL, NULL, "--online", NULL, "smash"},
    {"no --online: the file is not read", "smash.recomps.wtf:39421\n", NULL, "", NULL, "smash"},
    {"SSBU_NET_RELAY wins", "smash.recomps.wtf:39421\n", "SSBU_NET_RELAY=127.0.0.1:39420", "--online", "127.0.0.1:39420", "smash"},
    {"SSBU_NET_RELAY=off wins", "smash.recomps.wtf:39421\n", "SSBU_NET_RELAY=off", "--online", NULL, "smash"},
    {"hosting a private room: no lobby", "smash.recomps.wtf:39421\n", "SSBU_NET_HOST=1", "--online", NULL, "smash"},
};

/* The child: reads the settings as the game does and writes what it found to result.txt. */
static int child(int argc, char **argv) {
    settings_args(argc, argv);
    const char *a = net_relay_address();
    FILE *f = fopen("result.txt", "wb");
    if (!f) return 1;
    fprintf(f, "%s\n%s\n", a ? a : "(none)", room);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--child")) return child(argc, argv);
    static const char *const VARS[] = {"SSBU_NET_RELAY", "SSBU_NET_HOST", "SSBU_NET_ROOM", "SSBU_NET_NAME", "SSBU_NET_DIRECT"};
    for (int v = 0; v < (int)(sizeof VARS / sizeof *VARS); v++) SetEnvironmentVariableA(VARS[v], NULL);
    char exe[MAX_PATH], base[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    GetTempPathA(sizeof base, base);
    for (int k = 0; k < (int)(sizeof CASES / sizeof *CASES); k++) {
        const Case *t = &CASES[k];
        char dir[MAX_PATH + 32], file[MAX_PATH + 64], cmd[2 * MAX_PATH], env[128] = "";
        snprintf(dir, sizeof dir, "%snetcfg%lu_%d", base, GetCurrentProcessId(), k);
        CreateDirectoryA(dir, NULL);
        if (t->server_file) {
            snprintf(file, sizeof file, "%s\\online_server.txt", dir);
            FILE *f = fopen(file, "wb");
            fputs(t->server_file, f);
            fclose(f);
        }
        char *eq = NULL;
        if (t->env) { snprintf(env, sizeof env, "%s", t->env); eq = strchr(env, '='); *eq = 0; SetEnvironmentVariableA(env, eq + 1); }
        snprintf(cmd, sizeof cmd, "\"%s\" --child %s", exe, t->args);
        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi;
        DWORD code = 1;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 30000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        if (t->env) SetEnvironmentVariableA(env, NULL);
        char got_address[300] = "", got_room[64] = "";
        snprintf(file, sizeof file, "%s\\result.txt", dir);
        FILE *f = fopen(file, "rb");
        if (f) {
            if (fgets(got_address, sizeof got_address, f)) got_address[strcspn(got_address, "\r\n")] = 0;
            if (fgets(got_room, sizeof got_room, f)) got_room[strcspn(got_room, "\r\n")] = 0;
            fclose(f);
        }
        CHECK(code == 0 && f, "%s: the child did not run (exit %lu)", t->name, code);
        const char *want = t->address ? t->address : "(none)";
        CHECK(!strcmp(got_address, want), "%s: the lobby address is '%s', wanted '%s'", t->name, got_address, want);
        CHECK(!strcmp(got_room, t->room), "%s: the lobby code is '%s', wanted '%s'", t->name, got_room, t->room);
        DeleteFileA(file);
        snprintf(file, sizeof file, "%s\\online_server.txt", dir);
        DeleteFileA(file);
        snprintf(file, sizeof file, "%s\\online_name.txt", dir);   /* (picked by the game when online) */
        DeleteFileA(file);
        RemoveDirectoryA(dir);
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
