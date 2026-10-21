/* Netplay's relay (see net.h): the client every player's game runs, and the relay of a private
 * room, which the host player's game runs. tools/ssbu_relay.py is the online lobby: the same
 * protocol, plus room lists.
 *
 * Protocol 2, UDP, every field big-endian; each packet starts 'S' 'N' version type:
 *   1 JOIN     client: nonce u64, room[32], name[32], build[40]  (repeated until answered; build:
 *                      the game's main module build id - players of different versions never meet)
 *   2 WELCOME  relay:  nonce u64, address u32, cookie u64, flags u8 (1: a lobby), then the member
 *                      list as in MEMBERS (empty from a lobby)
 *   3 REJECT   relay:  nonce u64, reason u8, text[64]       (sent with the JOIN's version)
 *   4 PING     client: nonce u64, time u64, cookie u64, ping u16   (every second; keeps NAT open)
 *   5 PONG     relay:  time u64, players u16, rooms u16, flags u8
 *   6 DATA     both:   source u32, destination u32, source port u16, destination port u16, payload
 *   7 MEMBERS  relay:  count u8, count x (address u32, name[32])
 *   8 LEAVE    client: nonce u64
 *   9 LIST     client: cookie u64, seq u32                 (a lobby's rooms, for nn::ldn's Scan)
 *  10 LISTING  lobby:  seq u32, host address u32, NetworkInfo[0x480]   (up to 24, best first)
 *  11 LIST_END lobby:  seq u32, count u8
 *  12 PEERS    lobby:  cookie u64, version u32, count u8, count x (address u32, name[32], flags u8,
 *                      public address u32, port u16, LAN address u32, port u16, secret u64)
 *                      (the other players of this player's room; flags 1: introduced, 2: that player
 *                      turned direct connections off, 4: its game cannot make them)
 *  13 BCAST    client: count u8, count x address u32, then as DATA   (a broadcast for the room's
 *                      players not listed: the ones this player reaches directly)
 *  14 PUNCH    player: secret u64, from u32, to u32, ack u8, nonce u64   (straight to another player)
 *  15 DIRECT   player: secret u64, then as DATA                         (the same)
 * A game that can connect directly adds to its PINGs: peers version u32 (of the last PEERS it got),
 * LAN address u32, port u16, flags u8 (1: direct connections allowed), direct peers u8; a lobby
 * that introduces players says so with WELCOME flag 2. Older games and lobbies ignore both.
 * A relay gives each member of a room an address in 10.13.0.0/16 and forwards DATA by
 * destination (the room's broadcast address: every other member), checking the source; a
 * lobby gives every player an address of its own and carries a Local Wireless network's
 * traffic between that network's players only. A member that is silent for 10 s is gone; a
 * JOIN with its nonce from a new place takes it back. Up to 8 members a relay's room.
 *
 * Direct connections: the lobby introduces the players of a room to each other, and their games
 * then send the room's traffic straight to each other where their routers let them (UDP hole
 * punching), through the lobby where not. See "direct connections" below. */
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S
#define COBJMACROS
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <natupnp.h>
#include <oleauto.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "net.h"
#include "settings.h"

#define MAGIC0 'S'
#define MAGIC1 'N'
enum { T_JOIN = 1, T_WELCOME, T_REJECT, T_PING, T_PONG, T_DATA, T_MEMBERS, T_LEAVE, T_LIST, T_LISTING, T_LIST_END,
       T_PEERS, T_BCAST, T_PUNCH, T_DIRECT };
#define NAME_LEN 32
#define BUILD_LEN 40
#define JOIN_SIZE (4 + 8 + 2 * NAME_LEN + BUILD_LEN)
#define ROOM_MEMBERS 8
#define TIMEOUT_MS 10000
#define UNREACHABLE_MS 5000   /* unanswered this long: the game plays offline (and keeps asking) */
#define NETWORK_INFO 0x480
#define PING_EXT_SIZE (4 + 8 + 8 + 8 + 2 + 4 + 4 + 2 + 1 + 1)

extern const RtModule rt_module_main;   /* (its build id tells game versions apart) */

static u16 get16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 get32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u64 get64(const u8 *p) { return (u64)get32(p) << 32 | get32(p + 4); }
static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void put64(u8 *p, u64 v) { put32(p, (u32)(v >> 32)); put32(p + 4, (u32)v); }
static u64 now_ms(void) { return GetTickCount64(); }
static void header(u8 *p, int type) { p[0] = MAGIC0; p[1] = MAGIC1; p[2] = NET_PROTOCOL; p[3] = (u8)type; }

/* Every packet goes out here. macOS refuses packets to the local network (EHOSTUNREACH, or
 * EPERM) until the user lets the game reach it - macOS asks the first time; System Settings >
 * Privacy & Security > Local Network changes it later: the first refusal is told, with where. */
#ifdef __APPLE__
static int local_network(u32 ip) {   /* (host order) private, link-local, multicast, broadcast */
    return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE || (ip >> 28) == 0xE || ip == 0xFFFFFFFFu;
}
#endif
static void send_udp(SOCKET s, const void *p, int n, const struct sockaddr_in *to) {
    if (sendto(s, (const char *)p, n, 0, (const struct sockaddr *)to, sizeof *to) >= 0) return;
#ifdef __APPLE__
    static volatile LONG told;
    int err = errno;
    if ((err == EHOSTUNREACH || err == EPERM) && local_network(ntohl(to->sin_addr.s_addr)) && !InterlockedExchange(&told, 1)) {
        char ip[16];
        inet_ntop(AF_INET, &to->sin_addr, ip, sizeof ip);
        fprintf(stderr, "[net] macOS keeps the game off your local network (%s:%u: %s): allow WindowSmash in System Settings > "
                        "Privacy & Security > Local Network\n", ip, ntohs(to->sin_port), strerror(err));
    }
#endif
}

/* (POSIX) room in a socket's send buffer for the biggest datagram the game sends (net_send's
 * 65000 bytes and a header), as a bigger one is refused: macOS's buffer holds 9216 bytes to start
 * with (Linux's far more already; Windows' is left as it is). */
static void big_sends(SOCKET s) {
#ifndef _WIN32
    int size = 0;
    socklen_t len = sizeof size;
    if (getsockopt(s, SOL_SOCKET, SO_SNDBUF, &size, &len) || size < 65536) {
        size = 65536;
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
    }
#else
    (void)s;
#endif
}

/* ---- configuration ------------------------------------------------------------------ */
static int configured = -1, hosting, logging;
static char relay_text[256], room[NAME_LEN + 1] = "smash", name[NAME_LEN + 1] = "Player";
static u16 port = NET_DEFAULT_PORT;
static int direct_ok = 1, test_nat;   /* SSBU_NET_DIRECT; SSBU_NET_TEST_NAT (tests) */
static u64 test_block_at = ~0ull;     /* SSBU_NET_TEST_BLOCK (tests): no direct packets from then on */

#ifdef _WIN32
static u64 process_age_ms(void) {
    FILETIME created, x, y, z, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &x, &y, &z)) return 0;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = created.dwLowDateTime; a.HighPart = created.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? (b.QuadPart - a.QuadPart) / 10000 : 0;
}
#else
/* (Linux: the runtime's own clock, which starts with the process) */
static u64 process_age_ms(void) { return rt_now_ns() / 1000000; }
#endif

/* Started with --online (Play SSBU.bat; settings.h): the lobby, name and direct connections from
 * the files the settings menu and the launcher write, where no SSBU_NET_* variable says them.
 * online_server.txt: "host:port [lobby code]" or "off"; online_name.txt: the name (one is picked
 * and kept there when it is missing); online_direct.txt: "off" for no direct connections. */
static void configure_from_files(void) {
    char line[256], host[256] = "", code[NAME_LEN + 1] = "";
    if (settings_line_read("online_server.txt", line, sizeof line)) sscanf(line, "%255s %32s", host, code);
    if (!host[0] || !_stricmp(host, "off")) return;
    snprintf(relay_text, sizeof relay_text, "%s", host);
    if (code[0] && !getenv("SSBU_NET_ROOM")) snprintf(room, sizeof room, "%s", code);
    if (!getenv("SSBU_NET_NAME")) {
        if (!settings_line_read("online_name.txt", line, sizeof line) || !line[0]) {
            snprintf(line, sizeof line, "Player%04u", 1000 + (unsigned)(GetTickCount64() * 2654435761u >> 7) % 9000);
            settings_line_write("online_name.txt", line);
            fprintf(stderr, "[net] online as %s (change it in the settings: Esc)\n", line);
        }
        snprintf(name, sizeof name, "%s", line);
    }
    if (!getenv("SSBU_NET_DIRECT") && settings_line_read("online_direct.txt", line, sizeof line) && !_strnicmp(line, "off", 3)) direct_ok = 0;
}

static void configure(void) {
    if (configured >= 0) return;
    const char *h = getenv("SSBU_NET_HOST"), *r = getenv("SSBU_NET_RELAY"), *v;
    hosting = h && *h && strcmp(h, "0");
    if ((v = getenv("SSBU_NET_PORT")) && atoi(v) > 0 && atoi(v) < 65536) port = (u16)atoi(v);
    if ((v = getenv("SSBU_NET_ROOM")) && *v) snprintf(room, sizeof room, "%s", v);
    if ((v = getenv("SSBU_NET_NAME")) && *v) snprintf(name, sizeof name, "%s", v);
    if ((v = getenv("SSBU_NET_LOG"))) logging = atoi(v) > 0 ? atoi(v) : 1;
    if ((v = getenv("SSBU_NET_DIRECT")) && !strcmp(v, "0")) direct_ok = 0;
    /* (tests) SSBU_NET_TEST_NAT=1: let in direct packets only from addresses sent to lately, as a
     * router does; SSBU_NET_TEST_BLOCK=<seconds after the game started>: drop every direct packet
     * from then on, as if a firewall closed the path (0: from the start, as a symmetric NAT) */
    if ((v = getenv("SSBU_NET_TEST_NAT"))) test_nat = atoi(v) > 0;
    if ((v = getenv("SSBU_NET_TEST_BLOCK")) && *v) {
        u64 age = process_age_ms(), at = (u64)(atof(v) * 1000);
        test_block_at = now_ms() + (at > age ? at - age : 0);
    }
    if (hosting) snprintf(relay_text, sizeof relay_text, "127.0.0.1:%u", port);
    else if (r && *r) { if (_stricmp(r, "off")) snprintf(relay_text, sizeof relay_text, "%s", r); }
    else if (settings_online()) configure_from_files();
    configured = hosting || relay_text[0];
}

static void start(void);
int net_configured(void) { start(); return configured; }
int net_log(void) { configure(); return logging; }
const char *net_player_name(void) { configure(); return name; }
const char *net_relay_address(void) { configure(); return !hosting && relay_text[0] ? relay_text : NULL; }

/* ---- the relay (host mode) ------------------------------------------------------------ */
typedef struct Member { struct sockaddr_in at; u64 nonce, seen; u32 ip; char name[NAME_LEN + 1]; } Member;
typedef struct Room { char name[NAME_LEN + 1]; u8 build[BUILD_LEN]; Member members[ROOM_MEMBERS]; int count; } Room;
#define ROOMS 64
static Room rooms[ROOMS];
static SOCKET relay_sock = INVALID_SOCKET;

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int member_list(const Room *r, u8 *p) {
    p[0] = (u8)r->count;
    for (int i = 0; i < r->count; i++) {
        put32(p + 1 + i * (4 + NAME_LEN), r->members[i].ip);
        memcpy(p + 5 + i * (4 + NAME_LEN), r->members[i].name, NAME_LEN);
    }
    return 1 + r->count * (4 + NAME_LEN);
}

static void tell_members(const Room *r) {
    u8 p[4 + 1 + ROOM_MEMBERS * (4 + NAME_LEN)];
    header(p, T_MEMBERS);
    int n = 4 + member_list(r, p + 4);
    for (int i = 0; i < r->count; i++) send_udp(relay_sock, p, n, &r->members[i].at);
}

static void drop_member(Room *r, int i) {
    if (logging) fprintf(stderr, "[net] relay: %s left room %s\n", r->members[i].name, r->name);
    r->members[i] = r->members[--r->count];
    tell_members(r);
}

static void relay_packet(const u8 *p, int n, const struct sockaddr_in *from) {
    if (n < 4 || p[0] != MAGIC0 || p[1] != MAGIC1) return;
    u8 out[4 + 8 + 4 + 8 + 1 + 1 + ROOM_MEMBERS * (4 + NAME_LEN)];
    if (p[2] != NET_PROTOCOL) {   /* another version of the game's netplay: say so */
        if (n >= 4 + 8 + 2 * NAME_LEN && p[3] == T_JOIN) {   /* (only to a whole JOIN: no small packet gets a bigger answer) */
            memset(out, 0, 4 + 8 + 1 + 64);
            header(out, T_REJECT);
            memcpy(out + 4, p + 4, 8);
            out[12] = 2;
            snprintf((char *)out + 13, 64, "different netplay version (relay %d, you %d)", NET_PROTOCOL, p[2]);
            send_udp(relay_sock, out, 4 + 8 + 1 + 64, from);
        }
        return;
    }
    /* who sent it */
    Room *r = NULL;
    int m = -1;
    for (int i = 0; i < ROOMS && m < 0; i++)
        for (int j = 0; j < rooms[i].count; j++)
            if (same_addr(&rooms[i].members[j].at, from)) { r = &rooms[i]; m = j; break; }
    switch (p[3]) {
    case T_JOIN: {
        if (n < JOIN_SIZE) return;
        u64 nonce = get64(p + 4);
        const u8 *build = p + 12 + 2 * NAME_LEN;
        char want[NAME_LEN + 1] = {0}, who[NAME_LEN + 1] = {0};
        memcpy(want, p + 12, NAME_LEN);
        memcpy(who, p + 12 + NAME_LEN, NAME_LEN);
        if (m >= 0 && r->members[m].nonce != nonce) { drop_member(r, m); r = NULL; m = -1; }   /* (a new game from the same place) */
        if (m < 0) {   /* a member coming back from a new place, or a new one */
            for (int i = 0; i < ROOMS && m < 0; i++)
                for (int j = 0; j < rooms[i].count; j++)
                    if (rooms[i].members[j].nonce == nonce && !strcmp(rooms[i].name, want)) { r = &rooms[i]; m = j; r->members[j].at = *from; break; }
        }
        if (m < 0) {
            r = NULL;
            for (int i = 0; i < ROOMS && !r; i++) if (rooms[i].count && !strcmp(rooms[i].name, want)) r = &rooms[i];
            for (int i = 0; i < ROOMS && !r; i++) if (!rooms[i].count) { r = &rooms[i]; snprintf(r->name, sizeof r->name, "%s", want); }
            const char *why = !r ? "the relay is full" : r->count == ROOM_MEMBERS ? "the room is full (8 players)"
                            : r->count && memcmp(r->build, build, BUILD_LEN) ? "the host has another game version" : NULL;
            if (why) {
                memset(out, 0, 4 + 8 + 1 + 64);
                header(out, T_REJECT);
                put64(out + 4, nonce);
                out[12] = r && r->count < ROOM_MEMBERS ? 3 : 1;
                snprintf((char *)out + 13, 64, "%s", why);
                send_udp(relay_sock, out, 4 + 8 + 1 + 64, from);
                return;
            }
            if (!r->count) memcpy(r->build, build, BUILD_LEN);
            u32 ip = 0;   /* the lowest free address from 10.13.0.2 */
            for (u32 cand = NET_GATEWAY + 1; !ip; cand++) {
                int used = 0;
                for (int j = 0; j < r->count; j++) used |= r->members[j].ip == cand;
                if (!used) ip = cand;
            }
            m = r->count++;
            memset(&r->members[m], 0, sizeof r->members[m]);
            r->members[m].at = *from;
            r->members[m].nonce = nonce;
            r->members[m].ip = ip;
            snprintf(r->members[m].name, sizeof r->members[m].name, "%s", who);
            if (logging) fprintf(stderr, "[net] relay: %s joined room %s as 10.13.%u.%u\n", who, r->name, ip >> 8 & 255, ip & 255);
            tell_members(r);
        }
        r->members[m].seen = now_ms();
        header(out, T_WELCOME);
        put64(out + 4, nonce);
        put32(out + 12, r->members[m].ip);
        put64(out + 16, 0);   /* (cookie: a private room's relay checks none) */
        out[24] = 0;          /* (flags: not a lobby) */
        int len = 25 + member_list(r, out + 25);
        send_udp(relay_sock, out, len, from);
        return;
    }
    case T_PING:
        if (m < 0 || n < 20) return;
        r->members[m].seen = now_ms();
        header(out, T_PONG);
        memcpy(out + 4, p + 12, 8);
        put16(out + 12, (u16)r->count);
        put16(out + 14, 0);
        out[16] = 0;
        send_udp(relay_sock, out, 17, from);
        return;
    case T_LEAVE:
        if (m >= 0) drop_member(r, m);
        return;
    case T_DATA: {
        if (m < 0 || n < 16) return;
        Member *src = &r->members[m];
        src->seen = now_ms();
        if (get32(p + 4) != src->ip) return;   /* (only from its own address) */
        u32 dst = get32(p + 8);
        for (int j = 0; j < r->count; j++)
            if (j != m && (dst == NET_BROADCAST || dst == 0xFFFFFFFFu || r->members[j].ip == dst))
                send_udp(relay_sock, p, n, &r->members[j].at);
        return;
    }
    }
}

static DWORD WINAPI relay_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"netplay relay");
    static u8 buf[65536];
    u64 last_sweep = now_ms();
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int n = recvfrom(relay_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        if (n > 0) relay_packet(buf, n, &from);
        if (now_ms() - last_sweep >= 1000) {   /* members gone silent */
            last_sweep = now_ms();
            for (int i = 0; i < ROOMS; i++)
                for (int j = 0; j < rooms[i].count;)
                    if (now_ms() - rooms[i].members[j].seen > TIMEOUT_MS) drop_member(&rooms[i], j); else j++;
        }
    }
}

/* ---- UPnP: open the relay's port on the router ---------------------------------------- */
#ifdef _WIN32
static const CLSID CLSID_UPnPNAT_ = {0xAE1E00AA, 0x3FD5, 0x403C, {0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1}};
static const IID IID_IUPnPNAT_ = {0xB171C812, 0xCC76, 0x485A, {0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99}};
static IStaticPortMappingCollection *upnp_mappings;
#endif
static volatile LONG upnp_state;   /* 0 trying, 1 port open, 2 no UPnP router, 3 refused, 4 open but the router is behind another NAT */
static char lan_ip[64], public_ip[64];

static u32 outbound_address(void) {   /* the address this PC goes out on, host order (no packet is sent) */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    socklen_t len = sizeof a;
    u32 ip = 0;
    if (s != INVALID_SOCKET && !connect(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &len))
        ip = ntohl(a.sin_addr.s_addr);
    if (s != INVALID_SOCKET) closesocket(s);
    return ip;
}

static void lan_address(void) {
    u32 ip = outbound_address();
    if (ip) snprintf(lan_ip, sizeof lan_ip, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
}

#ifdef _WIN32
static void upnp_close(void) {
    if (!upnp_mappings) return;
    BSTR udp = SysAllocString(L"UDP");
    IStaticPortMappingCollection_Remove(upnp_mappings, port, udp);
    SysFreeString(udp);
}
#else
/* (linux/upnp.c: the router's port mapping through SSDP and SOAP, as Windows' UPnP NAT API does) */
int rt_upnp_add(u16 port, const char *lan_ip, const char *what, char *public_ip, size_t n);
void rt_upnp_remove(void);
static void upnp_close(void) { rt_upnp_remove(); }
#endif

/* Whether an address is one the internet cannot reach: private, or carrier-grade NAT's 100.64/10 -
 * a router reporting one of these as its own outside address sits behind another NAT. */
static int unreachable_address(const char *text) {
    unsigned a, b, cc, d;
    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &cc, &d) != 4) return 0;
    return a == 10 || (a == 172 && (b & 0xF0) == 16) || (a == 192 && b == 168) || (a == 100 && (b & 0xC0) == 64) || a == 0;
}

static void write_share_file(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/../../netplay_address.txt", rt_data_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    if (upnp_state == 1) fprintf(f, "Give your friends this address to join: %s:%u\n", public_ip[0] ? public_ip : "(your public IP)", port);
    else if (upnp_state == 4) fprintf(f, "Your router opened the port, but its own internet address (%s) is not reachable from outside:\n"
                                         "it is behind another router or your provider's NAT. Friends on the same network can join %s:%u.\n"
                                         "For friends elsewhere, use a VPN or tools/ssbu_relay.py on a reachable machine (see docs/MULTIPLAYER.md).\n",
                                      public_ip, lan_ip, port);
    else fprintf(f, "Your router did not open the port (UPnP %s).\nFriends on the same network can join %s:%u.\n"
                    "For friends elsewhere, run tools/ssbu_relay.py on a reachable machine (see docs/MULTIPLAYER.md).\n",
                 upnp_state == 2 ? "not available" : "refused", lan_ip, port);
    fprintf(f, "Room: %s\n", room);
    fclose(f);
}

static DWORD WINAPI upnp_thread(LPVOID unused) {
    (void)unused;
    lan_address();
#ifndef _WIN32
    LONG state = rt_upnp_add(port, lan_ip, "SSBU netplay relay", public_ip, sizeof public_ip);
    if (state == 1) {
        atexit(upnp_close);
        if (unreachable_address(public_ip)) state = 4;
    }
#else
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IUPnPNAT *nat = NULL;
    LONG state = 2;
    if (SUCCEEDED(CoCreateInstance(&CLSID_UPnPNAT_, NULL, CLSCTX_ALL, &IID_IUPnPNAT_, (void **)&nat)) && nat) {
        IStaticPortMappingCollection *maps = NULL;
        if (SUCCEEDED(IUPnPNAT_get_StaticPortMappingCollection(nat, &maps)) && maps) {   /* (NULL: no UPnP router) */
            WCHAR client[64];
            MultiByteToWideChar(CP_UTF8, 0, lan_ip, -1, client, 64);
            BSTR udp = SysAllocString(L"UDP"), who = SysAllocString(client), what = SysAllocString(L"SSBU netplay relay");
            IStaticPortMapping *map = NULL;
            IStaticPortMappingCollection_Remove(maps, port, udp);   /* (a stale one from an earlier run) */
            if (SUCCEEDED(IStaticPortMappingCollection_Add(maps, port, udp, port, who, VARIANT_TRUE, what, &map)) && map) {
                BSTR ext = NULL;
                if (SUCCEEDED(IStaticPortMapping_get_ExternalIPAddress(map, &ext)) && ext) {
                    WideCharToMultiByte(CP_UTF8, 0, ext, -1, public_ip, sizeof public_ip, NULL, NULL);
                    SysFreeString(ext);
                }
                IStaticPortMapping_Release(map);
                upnp_mappings = maps;
                atexit(upnp_close);
                state = unreachable_address(public_ip) ? 4 : 1;
            } else {
                state = 3;
                IStaticPortMappingCollection_Release(maps);
            }
            SysFreeString(udp); SysFreeString(who); SysFreeString(what);
        }
        IUPnPNAT_Release(nat);
    }
#endif
    InterlockedExchange(&upnp_state, state);
    if (state == 1) fprintf(stderr, "[net] UPnP: port %u opened; friends join %s:%u (LAN address %s)\n", port, public_ip, port, lan_ip);
    else if (state == 4) fprintf(stderr, "[net] UPnP: port %u opened, but the router's outside address %s is behind another NAT; LAN address %s\n", port, public_ip, lan_ip);
    else fprintf(stderr, "[net] UPnP: %s; LAN address %s, port %u\n", state == 2 ? "no UPnP router found" : "the router refused the port", lan_ip, port);
    write_share_file();
    return 0;
}

/* ---- the client ---------------------------------------------------------------------- */
static SOCKET client_sock = INVALID_SOCKET;
static struct sockaddr_in relay_at;
static volatile LONG joined;           /* WELCOME seen and not timed out */
static volatile LONG lobby;            /* ...from a lobby */
static volatile u32 my_ip;
static u64 nonce, cookie, last_pong, unjoined_since;
static volatile LONG members, rooms_open, ping_ms = -1;
static char reject_text[80], member_names[ROOM_MEMBERS][NAME_LEN + 1];

u32 net_local_ip(void) { start(); return joined ? my_ip : 0; }
int net_unreachable(void) { start(); return configured && !joined && now_ms() - unjoined_since > UNREACHABLE_MS; }
int net_lobby(void) { start(); return joined && lobby; }

/* ---- direct connections (the online lobby's rooms) -------------------------------------
 * The lobby introduces the players of a room to each other (PEERS): each one's internet address
 * as the lobby sees it (and its address on its own network, when two are behind the same router)
 * and a secret of the pair's. Both then punch through their routers: PUNCH hellos to each address
 * the other may be at, every 200 ms for 5 s, each answered by an ack that echoes its nonce - an
 * ack proves the path works both ways. While acks keep coming (a hello every 400 ms) the room's
 * game traffic to that player goes straight there (DIRECT); otherwise through the lobby, as without
 * direct connections, with a hello now and then in case the path opens. Only the addresses the
 * lobby gave are ever sent to or heard from, and a direct packet reaches the game only with the
 * pair's secret, the sender's own virtual address, ports of the game's (never local wireless's,
 * which always goes through the lobby, the judge of who is in which room) and the sender a node
 * of this player's network. */
#define PEERS_MAX (ROOM_MEMBERS - 1)
#define PEER_SIZE (4 + NAME_LEN + 1 + 4 + 2 + 4 + 2 + 8)
#define PUNCH_SIZE (4 + 8 + 4 + 4 + 1 + 8)
#define DIRECT_HEAD (4 + 8)
#define PUNCH_MS 5000    /* punching this long without an answer: through the lobby */
#define USABLE_MS 1200   /* a direct path is used while its last ack is younger than this */
enum { P_PUNCHING, P_DIRECT, P_RELAYED };
typedef struct Candidate {   /* an address a player may be reached at */
    struct sockaddr_in at;
    u64 nonce, asked, prev, prev_asked;   /* our last two hellos there, and when they went */
    u64 sent;                             /* (when anything last went there: SSBU_NET_TEST_NAT) */
} Candidate;
typedef struct Peer {
    u32 ip;
    char name[NAME_LEN + 1];
    int flags, introduced, state, via, ncand, acks;   /* via: the candidate that answers; acks: sent this second */
    u64 secret, t0, last_ack, next_hello, acks_at;
    LONG rtt;
    Candidate cand[2];                                /* its address through its router, then on its own network */
    volatile LONG in_direct, in_relayed, out_direct, out_relayed, dropped;
} Peer;
static SRWLOCK peers_lock = SRWLOCK_INIT;   /* (never held while calling hle_net, or printing) */
static Peer peers[PEERS_MAX];
static int npeers;
static u32 peers_version;
static volatile LONG direct_not_in_room;    /* direct packets from a player not in this player's network */
static int bound_loopback;
static u32 local_ip;                        /* this player's own address and port (told to the lobby) */
static u16 local_port;

typedef struct Notes { int n; char line[2 * PEERS_MAX + 1][200]; } Notes;   /* (lines printed once the lock is let go) */
static void note(Notes *ns, const char *fmt, ...) {
    if (ns->n >= (int)(sizeof ns->line / sizeof ns->line[0])) return;
    va_list a;
    va_start(a, fmt);
    vsnprintf(ns->line[ns->n++], sizeof ns->line[0], fmt, a);
    va_end(a);
}
static void print_notes(const Notes *ns) { for (int i = 0; i < ns->n; i++) fprintf(stderr, "[net] %s\n", ns->line[i]); }

static const char *addr_text(const struct sockaddr_in *a, char *buf) {   /* (buf: 24 bytes) */
    char ip[16];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
    snprintf(buf, 24, "%s:%u", ip, ntohs(a->sin_port));
    return buf;
}
static u64 random64(void) { unsigned int a, b; rand_s(&a); rand_s(&b); return (u64)a << 32 | b; }

static int usable(const Peer *q, u64 t) { return q->state == P_DIRECT && t - q->last_ack < USABLE_MS && t < test_block_at; }

static const char *peer_state(const Peer *q, u64 t, char *buf) {   /* (buf: 32 bytes) */
    if (usable(q, t)) { snprintf(buf, 32, "direct %ld ms", (long)q->rtt); return buf; }
    if (q->introduced) return q->state == P_PUNCHING ? "relayed (trying direct)" : "relayed";
    return !direct_ok ? "relayed (direct off here)" : q->flags & 2 ? "relayed (direct off)" : q->flags & 4 ? "relayed (older game)" : "relayed";
}

static void send_punch(Peer *q, int c, int ack, u64 word, u64 t) {   /* (word: a hello's nonce, or the one an ack echoes) */   /* (peers_lock held exclusively) */
    if (t >= test_block_at) return;
    u8 p[PUNCH_SIZE];
    header(p, T_PUNCH);
    put64(p + 4, q->secret);
    put32(p + 12, my_ip);
    put32(p + 16, q->ip);
    p[20] = (u8)ack;
    put64(p + 21, word);
    q->cand[c].sent = t;
    send_udp(client_sock, p, sizeof p, &q->cand[c].at);
}
static void hello(Peer *q, int c, u64 t) {
    Candidate *k = &q->cand[c];
    k->prev = k->nonce;
    k->prev_asked = k->asked;
    k->nonce = random64() | 1;
    k->asked = t;
    send_punch(q, c, 0, k->nonce, t);
}

/* (peers_lock held) The introduced player whose secret a direct packet carries, if it came from
 * one of that player's addresses (which one: *cand); else NULL. */
static Peer *peer_from(u64 secret, const struct sockaddr_in *from, int *cand) {
    for (int i = 0; i < npeers; i++) {
        Peer *q = &peers[i];
        if (!q->introduced || q->secret != secret) continue;
        for (int c = 0; c < q->ncand; c++)
            if (same_addr(&q->cand[c].at, from)) { *cand = c; return q; }
        return NULL;
    }
    return NULL;
}
static int nat_lets_in(const Peer *q, int c, u64 t) { return !test_nat || (q->cand[c].sent && t - q->cand[c].sent < 30000); }

static void clear_peers(void) {
    AcquireSRWLockExclusive(&peers_lock);
    npeers = 0;
    peers_version = 0;
    ReleaseSRWLockExclusive(&peers_lock);
}

/* A PEERS from the lobby: the room's players now. One already known (same address, same secret)
 * keeps its path; a new one, or one at a new address, is punched to. */
static void read_peers(const u8 *p, int n) {
    if (n < 5) return;
    u32 version = get32(p);
    int count = p[4];
    if ((i32)(version - peers_version) <= 0 || count > PEERS_MAX || 5 + count * PEER_SIZE > n) return;
    u64 t = now_ms();
    Peer next[PEERS_MAX];
    Notes ns = {0};
    memset(next, 0, sizeof next);
    AcquireSRWLockExclusive(&peers_lock);
    for (int i = 0; i < count; i++) {
        const u8 *e = p + 5 + i * PEER_SIZE, *a = e + 4 + NAME_LEN + 1;
        Peer *q = &next[i];
        q->ip = get32(e);
        for (int k = 0; k < NAME_LEN; k++) q->name[k] = e[4 + k] && e[4 + k] < 0x20 ? '?' : (char)e[4 + k];
        q->flags = e[4 + NAME_LEN];
        q->secret = get64(a + 12);
        q->introduced = (q->flags & 1) && q->secret && get32(a) && get16(a + 4) && direct_ok;
        if (q->introduced) {
            q->ncand = 1;
            q->cand[0].at.sin_family = AF_INET;
            q->cand[0].at.sin_addr.s_addr = htonl(get32(a));
            q->cand[0].at.sin_port = htons(get16(a + 4));
            if (get32(a + 6) && get16(a + 10) && (get32(a + 6) != get32(a) || get16(a + 10) != get16(a + 4))) {
                q->cand[1].at.sin_family = AF_INET;
                q->cand[1].at.sin_addr.s_addr = htonl(get32(a + 6));
                q->cand[1].at.sin_port = htons(get16(a + 10));
                q->ncand = 2;
            }
        }
        const Peer *old = NULL;
        for (int j = 0; j < npeers && !old; j++)
            if (peers[j].ip == q->ip && peers[j].secret == q->secret && peers[j].introduced == q->introduced) old = &peers[j];
        int same = old && old->ncand == q->ncand;
        for (int c = 0; same && c < q->ncand; c++) same = same_addr(&old->cand[c].at, &q->cand[c].at);
        if (old) {
            q->in_direct = old->in_direct; q->in_relayed = old->in_relayed; q->dropped = old->dropped;
            q->out_direct = old->out_direct; q->out_relayed = old->out_relayed; q->rtt = old->rtt;
        }
        if (same) {
            q->state = old->state; q->via = old->via; q->t0 = old->t0; q->last_ack = old->last_ack;
            q->next_hello = old->next_hello; q->acks_at = old->acks_at; q->acks = old->acks;
            memcpy(q->cand, old->cand, sizeof q->cand);
        } else if (q->introduced) {
            char at[24];
            q->state = P_PUNCHING;
            q->t0 = q->next_hello = t;
            note(&ns, "%s: introduced, at %s%s; punching through", q->name, addr_text(&q->cand[0].at, at),
                 q->ncand > 1 ? " (or on this network)" : "");
        } else q->state = P_RELAYED;
    }
    memcpy(peers, next, sizeof next);
    npeers = count;
    peers_version = version;
    ReleaseSRWLockExclusive(&peers_lock);
    print_notes(&ns);
}

/* (client thread, every 50 ms) Hellos due, paths gone quiet, punching given up. */
static void peers_tick(u64 t) {
    static u64 last_report;
    Notes ns = {0};
    AcquireSRWLockExclusive(&peers_lock);
    for (int i = 0; i < npeers; i++) {
        Peer *q = &peers[i];
        if (!q->introduced) continue;
        if (q->state == P_DIRECT && t - q->last_ack >= USABLE_MS) {
            q->state = P_RELAYED;
            q->t0 = t;   /* (and hellos every 400 ms for a while: it may come back) */
            note(&ns, "direct path to %s went quiet; using the relay", q->name);
        } else if (q->state == P_PUNCHING && t - q->t0 >= PUNCH_MS) {
            q->state = P_RELAYED;
            note(&ns, "no direct path to %s (a router or firewall blocks it); using the relay", q->name);
        }
        if (t >= q->next_hello) {
            q->next_hello = t + (q->state == P_PUNCHING ? 200 : q->state == P_DIRECT || t - q->t0 < PUNCH_MS ? 400 : 5000);
            if (q->state == P_DIRECT) hello(q, q->via, t);
            else for (int c = 0; c < q->ncand; c++) hello(q, c, t);
        }
    }
    if (logging && npeers && t - last_report >= 10000) {   /* (debugging: which way each player's traffic goes) */
        last_report = t;
        for (int i = 0; i < npeers; i++) {
            char st[32];
            const Peer *q = &peers[i];
            note(&ns, "peer %s: %s; in %ld direct + %ld relayed, out %ld direct + %ld relayed, %ld dropped", q->name,
                 peer_state(q, t, st), (long)q->in_direct, (long)q->in_relayed, (long)q->out_direct, (long)q->out_relayed, (long)q->dropped);
        }
    }
    ReleaseSRWLockExclusive(&peers_lock);
    print_notes(&ns);
}

/* A packet from another player, straight (not from the relay). */
static void direct_packet(const u8 *p, int n, const struct sockaddr_in *from) {
    u64 t = now_ms();
    int c = 0;
    if (t >= test_block_at || !lobby || n < DIRECT_HEAD) return;
    u64 secret = get64(p + 4);
    if (p[3] == T_DIRECT && n >= DIRECT_HEAD + 12) {
        const u8 *b = p + DIRECT_HEAD;
        u32 src = get32(b), dst = get32(b + 4);
        u16 sport = get16(b + 8), dport = get16(b + 10);
        int ok = 0;
        AcquireSRWLockShared(&peers_lock);
        Peer *q = peer_from(secret, from, &c);
        if (q && nat_lets_in(q, c, t)) {   /* (what a router would not let in is not counted) */
            ok = q->ip == src && (dst == my_ip || dst == NET_BROADCAST || dst == 0xFFFFFFFFu) &&
                 sport != NET_LDN_PORT && dport != NET_LDN_PORT;
            InterlockedIncrement(ok ? &q->in_direct : &q->dropped);
        }
        ReleaseSRWLockShared(&peers_lock);
        if (ok && ldn_in_network(src)) vsock_deliver(src, sport, dst, dport, b + 12, (u32)(n - DIRECT_HEAD - 12));
        else if (ok) InterlockedIncrement(&direct_not_in_room);
        return;
    }
    if (p[3] != T_PUNCH || n < PUNCH_SIZE) return;
    u32 from_ip = get32(p + 12), to_ip = get32(p + 16);
    int ack = p[20];
    u64 echo = get64(p + 21);
    Notes ns = {0};
    AcquireSRWLockExclusive(&peers_lock);
    Peer *q = peer_from(secret, from, &c);
    if (q && !nat_lets_in(q, c, t)) q = NULL;
    if (q && q->ip == from_ip && to_ip == my_ip) {
        Candidate *k = &q->cand[c];
        if (ack) {   /* (an answer to one of our latest hellos there, from there: the path works both ways) */
            u64 asked = echo == k->nonce ? k->asked : echo == k->prev ? k->prev_asked : 0;
            if (echo && asked && t - asked < 2000) {
                if (echo == k->nonce) k->nonce = 0; else k->prev = 0;   /* (each answer once) */
                q->rtt = (LONG)(t - asked);
                q->last_ack = t;
                if (q->state != P_DIRECT || (c == 1 && q->via == 0)) {   /* (its own network's address rather than through the router) */
                    char at[24];
                    q->state = P_DIRECT;
                    q->via = c;
                    note(&ns, "%s: direct, via %s %s (%ld ms)", q->name, c ? "LAN" : "public", addr_text(&k->at, at), (long)q->rtt);
                }
            }
        } else {
            if (t - q->acks_at >= 1000) { q->acks_at = t; q->acks = 0; }
            if (q->acks++ < 10) {
                send_punch(q, c, 1, echo, t);
                if (q->state != P_DIRECT) hello(q, c, t);   /* (theirs got here: ours may get there now) */
            }
        }
    } else if (q) InterlockedIncrement(&q->dropped);
    ReleaseSRWLockExclusive(&peers_lock);
    print_notes(&ns);
}

static void count_relayed_in(u32 src) {
    AcquireSRWLockShared(&peers_lock);
    for (int i = 0; i < npeers; i++) if (peers[i].ip == src) InterlockedIncrement(&peers[i].in_relayed);
    ReleaseSRWLockShared(&peers_lock);
}

static void local_address(void) {   /* this player's own address and port, for players behind the same router */
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    if (getsockname(client_sock, (struct sockaddr *)&a, &len) || !a.sin_port) return;
    local_ip = bound_loopback ? INADDR_LOOPBACK : outbound_address();
    local_port = ntohs(a.sin_port);
}

/* To one player (straight there while a direct path works, else through the lobby), or to the
 * room: straight to each player reached directly, through the lobby to the rest. */
void net_send(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len) {
    if (!joined || client_sock == INVALID_SOCKET || len > 65000) return;
    enum { ROOM = 4 + 1 + 4 * PEERS_MAX };   /* (before the DATA body: room for the longest header, BCAST's) */
    u8 stack[1600], *buf = ROOM + 12 + len <= sizeof stack ? stack : malloc(ROOM + 12 + len);
    if (!buf) return;
    u8 *body = buf + ROOM, *h;
    put32(body, src_ip);
    put32(body + 4, dst_ip);
    put16(body + 8, src_port);
    put16(body + 10, dst_port);
    memcpy(body + 12, data, len);
    int n = 12 + (int)len, broadcast = dst_ip == NET_BROADCAST || dst_ip == 0xFFFFFFFFu, nd = 0, relayed = 0;
    struct { struct sockaddr_in at; u64 secret; u32 ip; } direct[PEERS_MAX];
    if (src_port != NET_LDN_PORT && dst_port != NET_LDN_PORT) {   /* (local wireless's own: always through the lobby) */
        u64 t = now_ms();
        AcquireSRWLockShared(&peers_lock);
        for (int i = 0; i < npeers; i++) {
            Peer *q = &peers[i];
            if (!broadcast && q->ip != dst_ip) continue;
            if (usable(q, t)) {
                direct[nd].at = q->cand[q->via].at;
                direct[nd].secret = q->secret;
                direct[nd++].ip = q->ip;
                InterlockedIncrement(&q->out_direct);
            } else {
                relayed++;
                InterlockedIncrement(&q->out_relayed);
            }
        }
        ReleaseSRWLockShared(&peers_lock);
    }
    for (int i = 0; i < nd; i++) {
        h = body - DIRECT_HEAD;
        header(h, T_DIRECT);
        put64(h + 4, direct[i].secret);
        send_udp(client_sock, h, n + DIRECT_HEAD, &direct[i].at);
    }
    if (!nd || (broadcast && relayed)) {
        if (nd) {   /* (a broadcast some players got directly: the lobby leaves those out) */
            h = body - (5 + 4 * nd);
            header(h, T_BCAST);
            h[4] = (u8)nd;
            for (int i = 0; i < nd; i++) put32(h + 5 + 4 * i, direct[i].ip);
        } else {
            h = body - 4;
            header(h, T_DATA);
        }
        send_udp(client_sock, h, (int)(body - h) + n, &relay_at);
    }
    if (buf != stack) free(buf);
}

/* (ICMP "port unreachable" would otherwise make a UDP socket's next receive fail; Linux reports
 * that only for connected sockets) */
#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
static void no_connreset(SOCKET s) {
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bytes, NULL, NULL);
}
#else
static void no_connreset(SOCKET s) { (void)s; }
#endif

static int resolve_relay(void) {
    char hostpart[256];
    snprintf(hostpart, sizeof hostpart, "%s", relay_text);
    u16 rport = port;
    char *colon = strrchr(hostpart, ':');
    if (colon) { *colon = 0; if (atoi(colon + 1) > 0) rport = (u16)atoi(colon + 1); }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(hostpart, NULL, &hints, &res) || !res) return 0;
    relay_at = *(struct sockaddr_in *)res->ai_addr;
    relay_at.sin_port = htons(rport);
    freeaddrinfo(res);
    if (relay_at.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {   /* (the relay is here: listen on loopback only) */
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(client_sock, (struct sockaddr *)&a, sizeof a);
        bound_loopback = 1;
    }
    return 1;
}

static void send_join(void) {
    u8 p[JOIN_SIZE] = {0};
    header(p, T_JOIN);
    put64(p + 4, nonce);
    memcpy(p + 12, room, strlen(room));
    memcpy(p + 12 + NAME_LEN, name, strlen(name));
    const char *build = rt_module_main.build_id ? rt_module_main.build_id : "";
    size_t build_len = strlen(build);
    memcpy(p + 12 + 2 * NAME_LEN, build, build_len < BUILD_LEN ? build_len : BUILD_LEN);
    send_udp(client_sock, p, sizeof p, &relay_at);
}

void net_lobby_list(u32 seq) {
    if (!net_lobby()) return;
    u8 p[4 + 8 + 4];
    header(p, T_LIST);
    put64(p + 4, cookie);
    put32(p + 12, seq);
    send_udp(client_sock, p, sizeof p, &relay_at);
}

static void read_members(const u8 *p, int n) {
    int count = n >= 1 ? p[0] : 0;
    if (count > ROOM_MEMBERS) count = ROOM_MEMBERS;
    for (int i = 0; i < count && 1 + (i + 1) * (4 + NAME_LEN) <= n; i++) {
        memcpy(member_names[i], p + 5 + i * (4 + NAME_LEN), NAME_LEN);
        member_names[i][NAME_LEN] = 0;
    }
    InterlockedExchange(&members, count);
}

static DWORD WINAPI client_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"netplay client");
    static u8 buf[65536];
    while (!resolve_relay()) {
        snprintf(reject_text, sizeof reject_text, "cannot find the relay %s", relay_text);
        Sleep(3000);
    }
    reject_text[0] = 0;
    u64 last_join = 0, last_ping = 0, last_tick = 0;
    if (!direct_ok) fprintf(stderr, "[net] direct connections off (SSBU_NET_DIRECT=0): all through the relay\n");
    for (;;) {
        u64 t = now_ms();
        if (!joined && t - last_join >= 500) { send_join(); last_join = t; }
        if (joined && t - last_ping >= 1000) {
            u8 p[PING_EXT_SIZE];
            header(p, T_PING);
            put64(p + 4, nonce);
            put64(p + 12, t);
            put64(p + 20, cookie);
            put16(p + 28, ping_ms < 0 || ping_ms > 0xFFFF ? 0xFFFF : (u16)ping_ms);
            if (!local_port) local_address();
            int direct_now = 0;
            AcquireSRWLockShared(&peers_lock);
            for (int i = 0; i < npeers; i++) direct_now += usable(&peers[i], t);
            put32(p + 30, peers_version);
            ReleaseSRWLockShared(&peers_lock);
            put32(p + 34, local_ip);
            put16(p + 38, local_port);
            p[40] = (u8)direct_ok;
            p[41] = (u8)direct_now;
            send_udp(client_sock, p, sizeof p, &relay_at);
            last_ping = t;
        }
        if (joined && t - last_pong > TIMEOUT_MS) {
            unjoined_since = t;
            InterlockedExchange(&joined, 0);   /* the relay is gone: keep asking to join again */
            InterlockedExchange(&ping_ms, -1);
            clear_peers();
            fprintf(stderr, "[net] lost the relay %s; reconnecting\n", relay_text);
        }
        if (joined && t - last_tick >= 50) { peers_tick(t); last_tick = t; }
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int n = recvfrom(client_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        if (n < 4 || buf[0] != MAGIC0 || buf[1] != MAGIC1) continue;
        if (!same_addr(&from, &relay_at)) {   /* (another player, straight) */
            if (buf[2] == NET_PROTOCOL && joined) direct_packet(buf, n, &from);
            continue;
        }
        if (buf[2] != NET_PROTOCOL && buf[3] != T_REJECT) continue;   /* (a REJECT comes in our own version) */
        switch (buf[3]) {
        case T_DATA:
            if (n >= 16 && joined) {
                if (get16(buf + 12) != NET_LDN_PORT) count_relayed_in(get32(buf + 4));
                vsock_deliver(get32(buf + 4), get16(buf + 12), get32(buf + 8), get16(buf + 14), buf + 16, (u32)(n - 16));
            }
            break;
        case T_WELCOME:
            if (n < 26 || get64(buf + 4) != nonce) break;
            if (!joined) clear_peers();   /* (a new start: the lobby counts PEERS from 1 again) */
            my_ip = get32(buf + 12);
            cookie = get64(buf + 16);
            InterlockedExchange(&lobby, buf[24] & 1);
            read_members(buf + 25, n - 25);
            last_pong = now_ms();
            last_ping = 0;   /* (a ping at once: a lobby answers nothing else until it has seen the cookie) */
            reject_text[0] = 0;
            if (!joined && lobby) fprintf(stderr, "[net] joined the lobby on %s as 10.13.%u.%u (lobby code %s)\n", relay_text, my_ip >> 8 & 255, my_ip & 255, room);
            else if (!joined) fprintf(stderr, "[net] joined room %s on %s as 10.13.%u.%u (%ld players)\n", room, relay_text, my_ip >> 8 & 255, my_ip & 255, (long)members);
            InterlockedExchange(&joined, 1);
            break;
        case T_REJECT:
            if (n >= 13 + 1 && get64(buf + 4) == nonce) {
                snprintf(reject_text, sizeof reject_text, "%.64s", (const char *)buf + 13);
                fprintf(stderr, "[net] the relay refused: %s\n", reject_text);
                Sleep(2000);
            }
            break;
        case T_PONG:
            if (n >= 17) {
                last_pong = now_ms();
                InterlockedExchange(&ping_ms, (LONG)(last_pong - get64(buf + 4)));
                if (lobby) { InterlockedExchange(&members, get16(buf + 12)); InterlockedExchange(&rooms_open, get16(buf + 14)); }
            }
            break;
        case T_MEMBERS:
            read_members(buf + 4, n - 4);
            break;
        case T_LISTING:
            if (n >= 12 + NETWORK_INFO && lobby) ldn_listing(get32(buf + 4), get32(buf + 8), buf + 12);
            break;
        case T_LIST_END:
            if (n >= 9 && lobby) ldn_list_end(get32(buf + 4));
            break;
        case T_PEERS:
            if (n >= 4 + 8 + 5 && lobby && get64(buf + 4) == cookie) read_peers(buf + 12, n - 12);
            break;
        }
    }
}

static void leave(void) {
    if (!joined) return;
    u8 p[12];
    header(p, T_LEAVE);
    put64(p + 4, nonce);
    send_udp(client_sock, p, sizeof p, &relay_at);
}

/* Started on first use (the overlay asks every frame), once Winsock is up. */
static void start(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (!pending) return;
    configure();
    if (configured) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        DWORD timeout = 100;
#else
        struct timeval timeout = {0, 100000};
#endif
        unsigned int r1, r2;
        rand_s(&r1); rand_s(&r2);
        nonce = (u64)r1 << 32 | r2;
        unjoined_since = now_ms();
        if (hosting) {
            relay_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            struct sockaddr_in a = {0};
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            const char *only = getenv("SSBU_NET_BIND");   /* (an address to listen on alone, e.g. 127.0.0.1 for tests) */
            if (only && *only) inet_pton(AF_INET, only, &a.sin_addr);
            if (relay_sock == INVALID_SOCKET || bind(relay_sock, (struct sockaddr *)&a, sizeof a)) {
                snprintf(reject_text, sizeof reject_text, "cannot use port %u for the relay (in use?)", port);
                fprintf(stderr, "[net] %s\n", reject_text);
            } else {
                setsockopt(relay_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
                no_connreset(relay_sock);
                big_sends(relay_sock);
                CloseHandle(CreateThread(NULL, 0, relay_thread, NULL, 0, NULL));
                const char *upnp = getenv("SSBU_NET_UPNP");   /* 0: leave the router alone */
                if (upnp && !strcmp(upnp, "0")) { lan_address(); upnp_state = 3; }
                else CloseHandle(CreateThread(NULL, 0, upnp_thread, NULL, 0, NULL));
                fprintf(stderr, "[net] hosting the relay on UDP port %u, room %s\n", port, room);
            }
        }
        client_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (client_sock != INVALID_SOCKET) {
            setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
            no_connreset(client_sock);
            int big = 1 << 20;
            setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof big);
            big_sends(client_sock);
            CloseHandle(CreateThread(NULL, 0, client_thread, NULL, 0, NULL));
            atexit(leave);
        }
    }
    InitOnceComplete(&once, 0, NULL);
}

void net_status(char *out, size_t n) {
    start();
    if (!configured) { if (n) out[0] = 0; return; }
    if (reject_text[0] && !joined) { snprintf(out, n, "Netplay: %s", reject_text); return; }
    if (!joined && net_unreachable()) { snprintf(out, n, "Netplay: no answer from %s - playing offline (still trying)", relay_text); return; }
    if (!joined) { snprintf(out, n, "Netplay: connecting to %s...", relay_text); return; }
    if (lobby) {   /* (smash, the default code, is the public lobby) */
        char code[NAME_LEN + 16] = "";
        if (strcmp(room, "smash")) snprintf(code, sizeof code, " (lobby %s)", room);
        snprintf(out, n, "Online%s: %ld player%s, %ld room%s - you are %s, ping %ld ms", code, (long)members, members == 1 ? "" : "s",
                 (long)rooms_open, rooms_open == 1 ? "" : "s", name, (long)ping_ms);
        size_t l = strlen(out);   /* the players of this player's room: direct, or through the lobby */
        u64 t = now_ms();
        AcquireSRWLockShared(&peers_lock);
        for (int i = 0; i < npeers && l + 1 < n; i++) {
            char st[32];
            int w = snprintf(out + l, n - l, "%s%s %s", i ? ", " : "\nRoom: ", peers[i].name, peer_state(&peers[i], t, st));
            l = w < 0 ? n - 1 : l + (size_t)w < n ? l + (size_t)w : n - 1;
        }
        ReleaseSRWLockShared(&peers_lock);
        return;
    }
    char share[128] = "";
    if (hosting) {
        LONG u = upnp_state;
        if (u == 0) snprintf(share, sizeof share, "  | opening port %u on the router...", port);
        else if (u == 1) snprintf(share, sizeof share, "  | friends join %s:%u", public_ip[0] ? public_ip : "(your public IP)", port);
        else if (u == 4) snprintf(share, sizeof share, "  | router behind another NAT: only same-network friends can join (%s:%u)", lan_ip, port);
        else snprintf(share, sizeof share, "  | UPnP %s: only same-network friends can join (%s:%u)", u == 2 ? "unavailable" : "refused", lan_ip, port);
    }
    snprintf(out, n, "Netplay: room %s, %ld player%s, %s 10.13.%u.%u, ping %ld ms%s", room, (long)members, members == 1 ? "" : "s",
             name, my_ip >> 8 & 255, my_ip & 255, (long)ping_ms, share);
}
