/* Rollback build, M5 step 3: the link between two games: one UDP socket, not blocking, bound to
 * this game's port, sending to the peer's. Messages are rb_net's (hle_rb_net.h); this file only
 * moves bytes. (Apart from hle_rb_core.c so that winsock2.h comes before windows.h here and nowhere
 * else.)
 * M6, the internet: the peer is a name or an IPv4 address, with ":port" when its port isn't the
 * usual one, or "*": this game waits to be found (its router forwards the port here; the other game
 * is given this one's public address). The peer's address is learned from its messages: rbl_accept,
 * once the caller has found the last message valid, takes that message's sender as the peer (a router
 * may give the other game's messages another port than the one it was told; a waiting game knows no
 * address at all). Once there is a peer, messages from any other address are dropped unread; one from
 * the peer's address on another port is read, and taken only if it is valid.
 * M7, 3+ players: the host keeps one address per player (below). */
#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <natupnp.h>
#include <oleauto.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)   /* (mstcpip.h has it only for some Windows targets) */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SOCKET sock = INVALID_SOCKET;
static struct sockaddr_in last_from;
static long long sent, received, send_errors, dropped, overflowed;

/* M6: a thread takes each message off the socket as it arrives and notes when (ms, the performance counter's
 * clock, as hle_rb_core.c's now_ms); the game reads them from this queue at its own safe points. A round trip
 * measured when the game got round to reading the answer took up to a frame on each side with it (the two
 * games at 60 Hz in step, so the least of many readings kept it: 16.1 ms on a home network where the trip
 * is 1 ms). The queue keeps every sender: rbl_recv filters, so the peer is only ever touched by the game. */
#define QN 1024
#define QBYTES 2048
static struct { int len; struct sockaddr_in from; double at; char data[QBYTES]; } q[QN];
static unsigned q_head, q_tail;   /* (q_tail - q_head messages waiting) */
static SRWLOCK q_lock = SRWLOCK_INIT;
static HANDLE rx_thread;
static volatile LONG rx_stop;
static double last_at;
void rbl_close(void);
static double clock_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static DWORD WINAPI rx_main(LPVOID unused) {
    (void)unused;
    static char buf[QBYTES];
    while (!rx_stop) {
        fd_set r; FD_ZERO(&r); FD_SET(sock, &r);
        struct timeval tv = {0, 50000};   /* (to notice rx_stop) */
        if (select(0, &r, NULL, NULL, &tv) <= 0) continue;
        for (;;) {
            struct sockaddr_in from; int fl = sizeof from;
            int n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) break;   /* (WSAEWOULDBLOCK: all taken) */
            double at = clock_ms();
            AcquireSRWLockExclusive(&q_lock);
            if (q_tail - q_head < QN) {
                unsigned k = q_tail++ % QN;
                q[k].len = n; q[k].from = from; q[k].at = at; memcpy(q[k].data, buf, (size_t)n);
            } else overflowed++;
            ReleaseSRWLockExclusive(&q_lock);
        }
    }
    return 0;
}

static void show(const struct sockaddr_in *a, char *s, size_t n) {
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
    snprintf(s, n, "%s:%d", ip, ntohs(a->sin_port));
}

/* ---- M6: the waiting game asks its router to send its port here (UPnP) ---------------------------
 * Windows' own UPnP NAT API, as WindowSmash's netplay relay opens its port (runtime/net_relay.c): on a
 * thread of its own (finding the router takes seconds), the mapping UDP <port> -> this PC, and the router's
 * outside address, which is what the other player types. Said in the log and in build\rollback_address.txt
 * (the launcher shows it). A mapping left by an earlier run is replaced; this one is removed at exit.
 * SSBU_RB_UPNP=0: the router is left alone (a port forwarded by hand works as before). */
static volatile LONG upnp_state;   /* 0 asking, 1 open, 2 no UPnP router, 3 refused, 4 open but the router is behind another NAT */
static IStaticPortMappingCollection *upnp_maps;
static int upnp_port;
static char upnp_public[64], upnp_lan[64];
static const CLSID CLSID_UPnPNAT_ = {0xAE1E00AA, 0x3FD5, 0x403C, {0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1}};
static const IID IID_IUPnPNAT_ = {0xB171C812, 0xCC76, 0x485A, {0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99}};

static void upnp_lan_address(void) {   /* the address this PC goes out on (no packet is sent) */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    int len = sizeof a;
    if (s != INVALID_SOCKET && !connect(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &len))
        inet_ntop(AF_INET, &a.sin_addr, upnp_lan, sizeof upnp_lan);
    if (s != INVALID_SOCKET) closesocket(s);
}
/* an address the internet cannot reach: private, or carrier-grade NAT's 100.64/10 (as net_relay.c) */
static int upnp_unreachable(const char *t) {
    unsigned a, b, c, d;
    if (sscanf(t, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    return a == 10 || (a == 172 && (b & 0xF0) == 16) || (a == 192 && b == 168) || (a == 100 && (b & 0xC0) == 64) || a == 0;
}
static void upnp_remove(void) {
    if (!upnp_maps) return;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* (the window's thread, at close: the mapping object lives in the multithreaded apartment) */
    BSTR udp = SysAllocString(L"UDP");
    HRESULT r = IStaticPortMappingCollection_Remove(upnp_maps, upnp_port, udp);
    SysFreeString(udp);
    IStaticPortMappingCollection_Release(upnp_maps);
    upnp_maps = NULL;
    fprintf(stderr, "[rb] link: UPnP: the router's UDP port %d mapping %s\n", upnp_port, SUCCEEDED(r) ? "removed" : "could not be removed (the next start replaces it)");
}
/* (gpu_d3d11.c, as the window closes: the game ends with ExitProcess, which runs no atexit) */
void rbl_upnp_close(void) { upnp_remove(); }
static void upnp_report(LONG state) {
    /* (Host/Join: both addresses - friends on this network type the first, friends elsewhere the second) */
    char line[600];
    if (state == 1)
        snprintf(line, sizeof line, "Friends join with:\n  on this network: %s\n  elsewhere:       %s\n(the router opened UDP port %d for this PC)",
                 upnp_lan, upnp_public, upnp_port);
    else if (state == 4)
        snprintf(line, sizeof line, "Friends on this network join with: %s\nThe router opened UDP port %d, but its own outside address (%s) is "
                 "behind another NAT (the provider's): friends elsewhere can't reach this PC - let one of them host instead",
                 upnp_lan, upnp_port, upnp_public);
    else
        snprintf(line, sizeof line, "Friends on this network join with: %s\nThe router %s: for friends elsewhere, forward UDP port %d to this PC "
                 "(%s) on the router by hand, and give them this PC's public address (search \"what is my IP\")",
                 upnp_lan, state == 2 ? "has no UPnP (or it is off)" : "refused to open the port (one forwarded by hand already works)",
                 upnp_port, upnp_lan);
    fprintf(stderr, "[rb] link: UPnP: %s\n", line);
    FILE *f = fopen("build\\rollback_address.txt", "w");
    if (f) { fprintf(f, "%s\n", line); fclose(f); }
}
static DWORD WINAPI upnp_main(LPVOID unused) {
    (void)unused;
    upnp_lan_address();
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IUPnPNAT *nat = NULL;
    LONG state = 2;
    if (SUCCEEDED(CoCreateInstance(&CLSID_UPnPNAT_, NULL, CLSCTX_ALL, &IID_IUPnPNAT_, (void **)&nat)) && nat) {
        IStaticPortMappingCollection *maps = NULL;
        if (SUCCEEDED(IUPnPNAT_get_StaticPortMappingCollection(nat, &maps)) && maps) {   /* (NULL: no UPnP router) */
            WCHAR client[64];
            MultiByteToWideChar(CP_UTF8, 0, upnp_lan, -1, client, 64);
            BSTR udp = SysAllocString(L"UDP"), who = SysAllocString(client), what = SysAllocString(L"SSBU rollback netplay");
            IStaticPortMapping *map = NULL;
            IStaticPortMappingCollection_Remove(maps, upnp_port, udp);   /* (one left by an earlier run) */
            if (SUCCEEDED(IStaticPortMappingCollection_Add(maps, upnp_port, udp, upnp_port, who, VARIANT_TRUE, what, &map)) && map) {
                BSTR ext = NULL;
                if (SUCCEEDED(IStaticPortMapping_get_ExternalIPAddress(map, &ext)) && ext) {
                    WideCharToMultiByte(CP_UTF8, 0, ext, -1, upnp_public, sizeof upnp_public, NULL, NULL);
                    SysFreeString(ext);
                }
                IStaticPortMapping_Release(map);
                upnp_maps = maps;
                atexit(upnp_remove);
                state = upnp_unreachable(upnp_public) ? 4 : 1;
            } else {
                state = 3;
                IStaticPortMappingCollection_Release(maps);
            }
            SysFreeString(udp); SysFreeString(who); SysFreeString(what);
        }
        IUPnPNAT_Release(nat);
    }
    InterlockedExchange(&upnp_state, state);
    upnp_report(state);
    return 0;
}
static void upnp_start(int port) {
    const char *e = getenv("SSBU_RB_UPNP");
    if (e && *e == '0') { fprintf(stderr, "[rb] link: UPnP off (SSBU_RB_UPNP=0): the router is left alone\n"); return; }
    upnp_port = port;
    HANDLE t = CreateThread(NULL, 0, upnp_main, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
int rbl_upnp_state(void) { return (int)upnp_state; }

/* M7, 3+ players, through the host: the host (player 1) keeps an address for every other player, learned from its
 * messages (or, two players at home, given); every other game has one, the host's. rbl_send goes to all of them
 * (the host) or to the host (the others); rbl_send_to to one (the host passing a player's message on). */
#define LP 8
static struct sockaddr_in peers[LP];
static int have[LP];
static int is_host, link_players = 2;
void rbl_players(int n) { link_players = n < 2 ? 2 : n > LP ? LP : n; }   /* (the host: how many to take in) */

/* 1: ready. local: this game's player (0: the host). host: the other game's address - the host's, or (the host, two
 * players at home) player 2's: "127.0.0.1" for two games on one PC, a name, "1.2.3.4:7702", or "*" (the host: wait). */
int rbl_open(int local, int my_port, const char *host, int peer_port) {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) return 0;
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return 0;
    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);
    /* (a message to a port nobody has open yet - the other game still starting - comes back as an error on
     * the next recv, which stopped the reading there for the frame: told not to) */
    BOOL no = FALSE; DWORD got = 0;
    WSAIoctl(sock, SIO_UDP_CONNRESET, &no, sizeof no, NULL, 0, &got, NULL, NULL);
    /* (Windows' default receive buffer holds a few messages: while one game was busy running frames
     * again, the other's overflowed it, and it got 928 of some 2000 (net4): waits, and rollbacks as deep
     * as they go. 1 MB holds over 700 of the largest.) */
    int rcv = 1 << 20;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv, sizeof rcv);
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET; me.sin_port = htons((u_short)my_port); me.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&me, sizeof me)) {
        fprintf(stderr, "[rb] link: port %d is taken (error %d)\n", my_port, WSAGetLastError());
        closesocket(sock); sock = INVALID_SOCKET;
        return 0;
    }
    memset(peers, 0, sizeof peers); memset(have, 0, sizeof have);
    is_host = local == 0;
    q_head = q_tail = 0; rx_stop = 0;
    rx_thread = CreateThread(NULL, 0, rx_main, NULL, 0, NULL);
    if (rx_thread) SetThreadPriority(rx_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    if (!host || !*host || !strcmp(host, "*")) {
        if (!is_host) { fprintf(stderr, "[rb] link: only the host (player 1) can wait to be found: give the host's address\n"); rbl_close(); return 0; }
        fprintf(stderr, "[rb] link: UDP port %d, waiting for the other games to find this one (its router must send UDP port %d here)\n", my_port, my_port);
        upnp_start(my_port);
        return 1;
    }
    char name[256];
    snprintf(name, sizeof name, "%s", host);
    char *colon = strrchr(name, ':');
    if (colon) { *colon = 0; if (atoi(colon + 1) > 0) peer_port = atoi(colon + 1); }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(name, NULL, &hints, &res) || !res) {
        fprintf(stderr, "[rb] link: the other game's address \"%s\" is not one (error %d)\n", name, WSAGetLastError());
        rbl_close();
        return 0;
    }
    int k = is_host ? 1 : 0;   /* (the host given an address: player 2's; the others: the host's) */
    peers[k] = *(struct sockaddr_in *)res->ai_addr;
    peers[k].sin_port = htons((u_short)peer_port);
    freeaddrinfo(res);
    have[k] = 1;
    char t[64]; show(&peers[k], t, sizeof t);
    fprintf(stderr, "[rb] link: UDP port %d, %s %s (%s)\n", my_port, is_host ? "player 2 at" : "the host (player 1) at", t, host);
    return 1;
}
void rbl_close(void) {   /* (the link test: one link after another in one process) */
    if (rx_thread) { rx_stop = 1; WaitForSingleObject(rx_thread, INFINITE); CloseHandle(rx_thread); rx_thread = NULL; }
    if (sock != INVALID_SOCKET) closesocket(sock);
    sock = INVALID_SOCKET; memset(have, 0, sizeof have);
    sent = received = send_errors = dropped = overflowed = 0;
    q_head = q_tail = 0;
}
void rbl_send_to(int p, const void *buf, int len) {
    if (sock == INVALID_SOCKET || p < 0 || p >= LP || !have[p]) return;   /* (not found yet: nowhere to send) */
    if (sendto(sock, buf, len, 0, (struct sockaddr *)&peers[p], sizeof peers[p]) == len) sent++; else send_errors++;
}
void rbl_send(const void *buf, int len) {
    if (!is_host) { rbl_send_to(0, buf, len); return; }
    for (int p = 1; p < LP; p++) rbl_send_to(p, buf, len);
}
/* (a message from this address may be read: a known game's, or - the host, while it waits for some player -
 * anyone's: the caller takes its sender in only once the message is valid, rbl_accept) */
static int welcome(const struct sockaddr_in *from) {
    for (int p = 0; p < LP; p++) if (have[p] && peers[p].sin_addr.s_addr == from->sin_addr.s_addr) return 1;
    return is_host;   /* (Host/Join: the host reads a new address even with every player found - a friend whose game started
                       * again comes from another port - and the core takes it in only as a valid joiner's hello) */
}
/* one message waiting, into buf: its length, or 0 when none */
int rbl_recv(void *buf, int cap) {
    if (sock == INVALID_SOCKET) return 0;
    for (;;) {
        AcquireSRWLockExclusive(&q_lock);
        if (q_head == q_tail) { ReleaseSRWLockExclusive(&q_lock); return 0; }
        unsigned k = q_head++ % QN;
        struct sockaddr_in from = q[k].from;
        int n = q[k].len < cap ? q[k].len : cap;
        memcpy(buf, q[k].data, (size_t)n);
        double at = q[k].at;
        ReleaseSRWLockExclusive(&q_lock);
        if (!welcome(&from)) { dropped++; continue; }   /* (not one of the games) */
        last_from = from; last_at = at;
        received++;
        return n;
    }
}
/* when the last message rbl_recv gave arrived (ms, the performance counter's clock) */
double rbl_last_arrival_ms(void) { return last_at; }
/* The last message read was valid and player p's (as it says). The host: its sender is player p's address from now on
 * (a new player found, or one its router moved to another port); the others: every message comes through the host,
 * whose port is followed the same way. 1: new or moved; 0: as known; -1: refused (player p is known at another address,
 * or the address is another player's). */
int rbl_accept(int p) {
    int k = is_host ? p : 0;
    if (k < 0 || k >= LP || (is_host && (k == 0 || k >= link_players))) return -1;
    if (have[k] && last_from.sin_addr.s_addr == peers[k].sin_addr.s_addr && last_from.sin_port == peers[k].sin_port) return 0;
    if (have[k] && last_from.sin_addr.s_addr != peers[k].sin_addr.s_addr) return -1;
    for (int q = 0; q < LP; q++)
        if (q != k && have[q] && peers[q].sin_addr.s_addr == last_from.sin_addr.s_addr && peers[q].sin_port == last_from.sin_port) return -1;
    char was[64] = "", now[64];
    if (have[k]) show(&peers[k], was, sizeof was);
    show(&last_from, now, sizeof now);
    static int moves[LP];
    if (have[k] && ++moves[k] == 3)   /* (a router moves a port once in a while; two games set up as the same player trade it back and forth) */
        fprintf(stderr, "[rb] link: player %d keeps changing ports: are two games set up as player %d? Each PC needs its own player number\n", k + 1, k + 1);
    if (have[k]) fprintf(stderr, "[rb] link: player %d's messages come from %s, not %s (its router gave it another port): answering there\n", k + 1, now, was);
    else fprintf(stderr, "[rb] link: player %d found at %s\n", k + 1, now);
    peers[k] = last_from; have[k] = 1;
    return 1;
}
int rbl_has_peer(int p) { return p >= 0 && p < LP && have[p]; }
/* (Host/Join, the host) the player whose address (and port) the last message came from, or -1: a joiner not given a number yet */
int rbl_who(void) {
    for (int p = 1; p < LP; p++)
        if (have[p] && peers[p].sin_addr.s_addr == last_from.sin_addr.s_addr && peers[p].sin_port == last_from.sin_port) return p;
    return -1;
}
/* (the host) player p is the last message's sender from now on, whatever was known: a joiner given that number (again) */
void rbl_assign(int p) {
    if (!is_host || p < 1 || p >= LP) return;
    char now[64]; show(&last_from, now, sizeof now);
    fprintf(stderr, "[rb] link: a joining game at %s is player %d\n", now, p + 1);
    peers[p] = last_from; have[p] = 1;
}
void rbl_stats(long long *s, long long *r, long long *e) { *s = sent; *r = received; *e = send_errors; }
long long rbl_dropped(void) { return dropped; }
