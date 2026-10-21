/* The game's network, natively: nn::nifm (is there a network, which address is mine),
 * nn::ldn (local wireless: networks hosted, found and joined over the virtual LAN - what the
 * game's Local Wireless rooms run on) and nn::socket, the BSD socket API, here a small UDP
 * stack on the virtual LAN of runtime/net.h.
 *
 * Without netplay configured the network is unavailable, as on a console that is offline.
 *
 * The socket API's numbers are the console's: addresses are FreeBSD sockaddr_in (with a
 * length byte), options, poll events and message flags have FreeBSD values, and errno has
 * Linux values (the SDK sets EAGAIN as 11) - errno is the C library's, through
 * __errno_location, as the SDK keeps it. */
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "net.h"
#include "online.h"

#define R_UNAVAILABLE RESULT(124, 300)
#define R_LDN_STATE   RESULT(203, 32)
#define CREATE_SYSTEM_EVENT "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb"
#define SIGNAL_SYSTEM_EVENT "_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE"

enum { E_BADF = 9, E_AGAIN = 11, E_INVAL = 22, E_MFILE = 24, E_DESTADDRREQ = 89, E_MSGSIZE = 90, E_NOPROTOOPT = 92,
       E_PROTONOSUPPORT = 93, E_OPNOTSUPP = 95, E_AFNOSUPPORT = 97, E_ADDRINUSE = 98, E_ADDRNOTAVAIL = 99,
       E_NETUNREACH = 101, E_NOTCONN = 107, E_CONNREFUSED = 111 };
enum { AF_INET_ = 2, SOCK_STREAM_ = 1, SOCK_DGRAM_ = 2 };
enum { SOL_SOCKET_ = 0xFFFF, SO_REUSEADDR_ = 0x4, SO_BROADCAST_ = 0x20, SO_REUSEPORT_ = 0x200, SO_SNDBUF_ = 0x1001,
       SO_RCVBUF_ = 0x1002, SO_SNDTIMEO_ = 0x1005, SO_RCVTIMEO_ = 0x1006, SO_ERROR_ = 0x1007, SO_TYPE_ = 0x1008 };
enum { F_GETFL_ = 3, F_SETFL_ = 4, O_NONBLOCK_ = 0x800, O_NONBLOCK_BSD = 0x4, SOCK_NONBLOCK_ = 0x20000000 };
enum { MSG_PEEK_ = 0x2, MSG_DONTWAIT_ = 0x80 };
enum { POLLIN_ = 0x1, POLLPRI_ = 0x2, POLLOUT_ = 0x4, POLLERR_ = 0x8, POLLHUP_ = 0x10, POLLNVAL_ = 0x20, POLLRDNORM_ = 0x40 };
#define LOOPBACK 0x7F000001u
#define ANY_BROADCAST 0xFFFFFFFFu

/* ---- nn::nifm --------------------------------------------------------------------- */
static int network_up(void) { return net_configured() && net_local_ip() != 0; }

HLE(_ZN2nn4nifm10InitializeEv) { c->x[0] = 0; }
HLE(_ZN2nn4nifm18IsNetworkAvailableEv) {
    c->x[0] = network_up();
    if (net_log() > 2) fprintf(stderr, "[net] nifm: network available: %llu\n", c->x[0]);
}
HLE(_ZN2nn4nifm22IsNetworkRequestOnHoldEv) { c->x[0] = 0; }
HLE(_ZN2nn4nifm20SubmitNetworkRequestEv) { (void)c; }
HLE(_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv) {
    /* (a request waits a little for the relay, as one waits for a connection - unless it has not
     * answered for a while: then the game is offline at once, not after a wait each time) */
    for (int i = 0; i < 50 && net_configured() && !net_local_ip() && !net_unreachable(); i++) Sleep(100);
    (void)c;
}
HLE(_ZN2nn4nifm20CancelNetworkRequestEv) { (void)c; }
HLE(_ZN2nn4nifm31HandleNetworkRequestErrorResultEv) { c->x[0] = network_up() ? 0 : R_UNAVAILABLE; }
HLE(_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_) {   /* address, mask, gateway, two DNS servers */
    if (!network_up()) { c->x[0] = R_UNAVAILABLE; return; }
    u32 v[5] = {net_local_ip(), NET_MASK, NET_GATEWAY, NET_GATEWAY, 0};
    for (int i = 0; i < 5; i++) if (c->x[i]) st32(c->x[i], _byteswap_ulong(v[i]));
    if (net_log()) fprintf(stderr, "[net] nifm: address %u.%u.%u.%u\n", v[0] >> 24, v[0] >> 16 & 255, v[0] >> 8 & 255, v[0] & 255);
    c->x[0] = 0;
}
HLE(_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr) {
    if (net_log()) fprintf(stderr, "[net] nifm: primary address asked (%s)\n", network_up() ? "up" : "down");
    if (!network_up()) { c->x[0] = R_UNAVAILABLE; return; }
    st32(c->x[0], _byteswap_ulong(net_local_ip()));
    c->x[0] = 0;
}
HLE(_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE) { c->x[0] = R_UNAVAILABLE; }

/* (debugging: every failure the game records in an nn::err::ErrorResultVariant, with where)
 * The addresses are 13.0.5's; the recompiler checks that the function there begins as the
 * signature on the line before says (tools/recomp.py wrap-sig prints one). */
/* signature: 320003e8 b9000008 b9400028 b9000408 d65f03c0 */
HLE_WRAP(7109199790) {
    u32 r = c->x[1] ? ld32(c->x[1]) : 0;
    if (r && net_log() > 1) {
        fprintf(stderr, "[net] error result %#x (module %u, description %u) recorded, from:\n", r, r & 0x1FF, r >> 9 & 0x1FFF);
        rt_backtrace();
    }
    f_7109199790_orig(c);
}
/* (and every Pia job - room creation, joining, ... - that ends in failure: Pia's own result
 * codes, which go nowhere near ErrorResultVariant) */
/* signature: d10143ff a9034ff4 a9047bfd 910103fd aa0003f3 910043e0 9....... 91001274 910043e1 aa1403e0 9....... 320007e8 910003e0 aa1403e1 b9000268 9....... */
HLE_WRAP(71000b41f0) {
    if (net_log() > 1) {
        u32 r = c->x[1] ? ld32(c->x[1]) : 0;
        fprintf(stderr, "[net] pia job failed: result %u (%#x), from:\n", r, r);
        rt_backtrace();
    }
    f_71000b41f0_orig(c);
}

/* ---- nn::ldn: local wireless, emulated on the virtual LAN ------------------------------
 * A host's network is its NetworkInfo - the console's structure: ids, SSID, the nodes (each
 * player's virtual address, MAC, name) and the advertise data the game sets. The host sends it
 * to the room every second from a port of the runtime's own (LDN_PORT); scanning lists the
 * networks seen lately. A station joins by asking the host, which adds its node and sends the
 * network again; stations keep telling the host they are there, the host keeps advertising,
 * and either side going quiet for a few seconds ends the connection. The game's own traffic
 * then goes between node addresses - virtual addresses - through its sockets. */
#define LDN_PORT NET_LDN_PORT
#define LDN_TRACE do { if (net_log() > 1) fprintf(stderr, "[net] ldn %s\n", __func__ + 14); } while (0)
#define R_LDN_CONNECT RESULT(203, 64)
enum { LS_NONE, LS_INITIALIZED, LS_AP, LS_AP_CREATED, LS_STATION, LS_CONNECTED };
enum { LM_ADVERTISE = 1, LM_JOIN, LM_ACCEPT, LM_REJECT, LM_KEEP, LM_LEAVE, LM_GONE, LM_KICK };
enum { DR_NONE, DR_USER, DR_SYSTEM, DR_DESTROYED_BY_USER, DR_DESTROYED_BY_SYSTEM, DR_REJECTED, DR_SIGNAL_LOST };

/* NetworkInfo, 0x480 bytes: intent id, session id, common info (BSSID, SSID, channel, link
 * level, type), then the LDN part (security parameter data and mode, accept policy, node
 * count and maximum, 8 nodes, advertise data). A node, 0x40 bytes: address, MAC, id,
 * connected, user name (32 + NUL), local communication version. */
#define NI_SIZE      0x480
#define NI_LCID      0x00
#define NI_SCENE     0x0A
#define NI_SESSION   0x10
#define NI_BSSID     0x20
#define NI_SSID_LEN  0x26
#define NI_SSID      0x27
#define NI_CHANNEL   0x48
#define NI_LINK      0x4A
#define NI_TYPE      0x4B
#define NI_SECPARAM  0x50
#define NI_SECMODE   0x60
#define NI_POLICY    0x62
#define NI_NODE_MAX  0x66
#define NI_NODE_COUNT 0x67
#define NI_NODES     0x68
#define NI_ADV_SIZE  0x26A
#define NI_ADV       0x26C
#define NODE(i)      (NI_NODES + 0x40 * (i))
#define SEEN_NETWORKS 24   /* (as many as a lobby lists) */
#define LIST_WAIT_MS 500   /* a scan waits this long at most for the lobby's list */

typedef struct SeenNetwork { u8 info[NI_SIZE]; u32 from; u64 at; } SeenNetwork;
static struct {
    SRWLOCK lock;
    CONDITION_VARIABLE changed;
    int state, reason, join_answer, agent;
    u32 event;                        /* the kernel event behind the game's state change event */
    u8 net[NI_SIZE];                  /* the network: this player's (host) or the one joined (station) */
    u64 node_heard[8], host_heard;    /* (host) when each station last spoke; (station) the host */
    u32 join_host;                    /* (station, joining) the host asked, which alone may answer */
    u8 join_session[16];
    SeenNetwork seen[SEEN_NETWORKS];  /* networks other players advertise, or the lobby lists */
    u32 list_asked, list_filling, list_done;   /* (lobby) the newest list asked for, being filled, complete */
    int list_next;
} L = {SRWLOCK_INIT, CONDITION_VARIABLE_INIT};

static u64 now_ms_(void) { return rt_now_ns() / 1000000; }
static u32 get_u32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static void put_u32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static u16 get_u16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }
static void put_u16(u8 *p, u16 v) { memcpy(p, &v, 2); }
static void make_mac(u8 *mac, u32 ip) { mac[0] = 0x02; mac[1] = 0x00; put_u32(mac + 2, _byteswap_ulong(ip)); }

static void ldn_signal(void) { if (L.event) kernel_signal_event(L.event); }
static void ldn_state_to(int state) {   /* (lock held) */
    if (net_log() && L.state != state) fprintf(stderr, "[net] ldn: state %d -> %d\n", L.state, state);
    L.state = state;
    ldn_signal();
}

static void ldn_send(u32 to, int type, const void *payload, u32 len) {
    u8 buf[4 + NI_SIZE + 4];
    if (len > sizeof buf - 4) return;
    buf[0] = 'L'; buf[1] = (u8)type; buf[2] = buf[3] = 0;
    memcpy(buf + 4, payload, len);
    net_send(net_local_ip(), LDN_PORT, to, LDN_PORT, buf, 4 + len);
}

static int node_of(u32 ip) {   /* (lock held) the node with this address, or -1 */
    for (int i = 0; i < 8; i++)
        if (L.net[NODE(i) + 0xB] && get_u32(L.net + NODE(i)) == ip) return i;
    return -1;
}

static void drop_node(int i) {   /* (host, lock held) */
    if (net_log()) fprintf(stderr, "[net] ldn: %.32s left the network\n", (const char *)L.net + NODE(i) + 0xC);
    memset(L.net + NODE(i), 0, 0x40);
    L.net[NI_NODE_COUNT]--;
    ldn_send(NET_BROADCAST, LM_ADVERTISE, L.net, NI_SIZE);
    ldn_signal();
}

static void lost_network(int reason) {   /* (station, lock held) */
    if (net_log()) fprintf(stderr, "[net] ldn: disconnected (reason %d)\n", reason);
    L.reason = reason;
    ldn_state_to(LS_STATION);
}

static DWORD WINAPI ldn_agent(LPVOID unused) {   /* advertising, keeping in touch, noticing who has gone */
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"netplay local wireless");
    u64 last = 0;
    for (;;) {
        Sleep(250);
        u64 t = now_ms_();
        AcquireSRWLockExclusive(&L.lock);
        int tick = t - last >= 1000;
        if (tick) last = t;
        if (L.state == LS_AP_CREATED) {
            if (tick) ldn_send(NET_BROADCAST, LM_ADVERTISE, L.net, NI_SIZE);
            for (int i = 1; i < 8; i++)
                if (L.net[NODE(i) + 0xB] && t - L.node_heard[i] > 6000) drop_node(i);
        } else if (L.state == LS_CONNECTED) {
            if (tick) ldn_send(get_u32(L.net + NODE(0)), LM_KEEP, L.net + NI_SESSION, 16);
            if (t - L.host_heard > 6000) lost_network(DR_SIGNAL_LOST);
        }
        ReleaseSRWLockExclusive(&L.lock);
    }
}

/* (vsock_deliver) a message to this player's local wireless from another's */
static void ldn_receive(u32 from, const u8 *p, u32 n) {
    if (n < 4 || p[0] != 'L') return;
    const u8 *m = p + 4;
    u32 len = n - 4;
    u64 t = now_ms_();
    AcquireSRWLockExclusive(&L.lock);
    switch (p[1]) {
    case LM_ADVERTISE: {
        if (len < NI_SIZE || get_u32(m + NODE(0)) != from) break;   /* (only a network's host advertises it) */
        int slot = 0;   /* the same network seen before, else the oldest entry */
        for (int i = 0; i < SEEN_NETWORKS; i++) {
            if (!memcmp(L.seen[i].info + NI_SESSION, m + NI_SESSION, 16)) { slot = i; break; }
            if (L.seen[i].at < L.seen[slot].at) slot = i;
        }
        memcpy(L.seen[slot].info, m, NI_SIZE);
        L.seen[slot].from = from;
        L.seen[slot].at = t;
        if (L.state == LS_CONNECTED && !memcmp(L.net + NI_SESSION, m + NI_SESSION, 16) && from == get_u32(L.net + NODE(0))) {
            L.host_heard = t;
            if (memcmp(L.net, m, NI_SIZE)) {   /* players came or went */
                memcpy(L.net, m, NI_SIZE);
                if (node_of(net_local_ip()) < 0) lost_network(DR_REJECTED);
                else ldn_signal();
            }
        }
        break;
    }
    case LM_JOIN: {   /* session[16], user name[32], version s16 */
        if (len < 50 || L.state != LS_AP_CREATED || memcmp(m, L.net + NI_SESSION, 16)) break;
        int node = node_of(from);
        if (node < 0 && L.net[NI_POLICY] != 1 && L.net[NI_NODE_COUNT] < L.net[NI_NODE_MAX])
            for (int i = 1; i < 8 && node < 0; i++)
                if (!L.net[NODE(i) + 0xB]) {
                    node = i;
                    u8 *nd = L.net + NODE(i);
                    memset(nd, 0, 0x40);
                    put_u32(nd, from);
                    make_mac(nd + 4, from);
                    nd[0xA] = (u8)i;
                    nd[0xB] = 1;
                    memcpy(nd + 0xC, m + 16, 32);
                    memcpy(nd + 0x2E, m + 48, 2);
                    L.net[NI_NODE_COUNT]++;
                    if (net_log()) fprintf(stderr, "[net] ldn: %.32s joined as node %d\n", (const char *)nd + 0xC, i);
                }
        if (node < 0) { u8 why = 1; ldn_send(from, LM_REJECT, &why, 1); break; }
        L.node_heard[node] = t;
        ldn_send(from, LM_ACCEPT, L.net, NI_SIZE);
        ldn_send(NET_BROADCAST, LM_ADVERTISE, L.net, NI_SIZE);
        ldn_signal();
        break;
    }
    case LM_ACCEPT:   /* (only from the host asked, for the network asked for) */
        if (len < NI_SIZE || L.state != LS_STATION || L.join_answer || from != L.join_host || memcmp(m + NI_SESSION, L.join_session, 16)) break;
        memcpy(L.net, m, NI_SIZE);
        L.join_answer = 1;
        WakeAllConditionVariable(&L.changed);
        break;
    case LM_REJECT:
        if (L.state == LS_STATION && !L.join_answer && from == L.join_host) { L.join_answer = -1; WakeAllConditionVariable(&L.changed); }
        break;
    case LM_KEEP: {
        int node = L.state == LS_AP_CREATED && len >= 16 && !memcmp(m, L.net + NI_SESSION, 16) ? node_of(from) : -1;
        if (node > 0) L.node_heard[node] = t;
        break;
    }
    case LM_LEAVE: {
        int node = L.state == LS_AP_CREATED && len >= 16 && !memcmp(m, L.net + NI_SESSION, 16) ? node_of(from) : -1;
        if (node > 0) drop_node(node);
        break;
    }
    case LM_GONE:
    case LM_KICK:   /* (only from the host) */
        if (L.state == LS_CONNECTED && len >= 16 && !memcmp(m, L.net + NI_SESSION, 16) && from == get_u32(L.net + NODE(0)))
            lost_network(p[1] == LM_GONE ? DR_DESTROYED_BY_USER : DR_REJECTED);
        break;
    }
    ReleaseSRWLockExclusive(&L.lock);
}

/* (net_relay.c) A room in the online lobby's list asked for as seq: the list replaces the
 * networks seen, in its order (the best rooms first), and its end wakes the scan waiting for it. */
static void start_list(u32 seq) {   /* (lock held) */
    if (L.list_filling == seq) return;
    memset(L.seen, 0, sizeof L.seen);
    L.list_filling = seq;
    L.list_next = 0;
}
void ldn_listing(u32 seq, u32 owner, const u8 *info) {
    AcquireSRWLockExclusive(&L.lock);
    if (seq == L.list_asked && seq != L.list_done && get_u32(info + NODE(0)) == owner) {
        start_list(seq);
        if (L.list_next < SEEN_NETWORKS) {
            SeenNetwork *s = &L.seen[L.list_next++];
            memcpy(s->info, info, NI_SIZE);
            s->from = owner;
            s->at = now_ms_();
        }
    }
    ReleaseSRWLockExclusive(&L.lock);
}
void ldn_list_end(u32 seq) {
    AcquireSRWLockExclusive(&L.lock);
    if (seq == L.list_asked) {
        start_list(seq);   /* (an empty list: no rooms) */
        L.list_done = seq;
        WakeAllConditionVariable(&L.changed);
    }
    ReleaseSRWLockExclusive(&L.lock);
}

HLE(_ZN2nn3ldn10InitializeEv) { LDN_TRACE;
    if (!net_configured() || net_unreachable()) { c->x[0] = R_UNAVAILABLE; return; }   /* (offline, as without netplay) */
    AcquireSRWLockExclusive(&L.lock);
    if (!L.agent) { L.agent = 1; CloseHandle(CreateThread(NULL, 0, ldn_agent, NULL, 0, NULL)); }
    if (L.state == LS_NONE) L.state = LS_INITIALIZED;
    ReleaseSRWLockExclusive(&L.lock);
    c->x[0] = 0;
}
HLE(_ZN2nn3ldn8FinalizeEv) { LDN_TRACE;
    AcquireSRWLockExclusive(&L.lock);
    if (L.state == LS_AP_CREATED) ldn_send(NET_BROADCAST, LM_GONE, L.net + NI_SESSION, 16);
    if (L.state == LS_CONNECTED) ldn_send(get_u32(L.net + NODE(0)), LM_LEAVE, L.net + NI_SESSION, 16);
    L.state = LS_NONE;
    ReleaseSRWLockExclusive(&L.lock);
    (void)c;
}
HLE(_ZN2nn3ldn8GetStateEv) { LDN_TRACE; c->x[0] = (u32)L.state; }
HLE(_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE) { LDN_TRACE;
    L.event = rt_attach_system_event(c, c->x[0], 1);
}
HLE(_ZN2nn3ldn11OpenStationEv) { LDN_TRACE;
    AcquireSRWLockExclusive(&L.lock);
    int ok = L.state == LS_INITIALIZED || L.state == LS_STATION;
    if (ok) { L.reason = DR_NONE; ldn_state_to(LS_STATION); }
    ReleaseSRWLockExclusive(&L.lock);
    c->x[0] = ok ? 0 : R_LDN_STATE;
}
HLE(_ZN2nn3ldn12CloseStationEv) { LDN_TRACE;
    AcquireSRWLockExclusive(&L.lock);
    if (L.state == LS_CONNECTED) ldn_send(get_u32(L.net + NODE(0)), LM_LEAVE, L.net + NI_SESSION, 16);
    if (L.state == LS_STATION || L.state == LS_CONNECTED) ldn_state_to(LS_INITIALIZED);
    ReleaseSRWLockExclusive(&L.lock);
    c->x[0] = 0;
}
HLE(_ZN2nn3ldn15OpenAccessPointEv) { LDN_TRACE;
    AcquireSRWLockExclusive(&L.lock);
    int ok = L.state == LS_INITIALIZED || L.state == LS_AP;
    if (ok) ldn_state_to(LS_AP);
    ReleaseSRWLockExclusive(&L.lock);
    c->x[0] = ok ? 0 : R_LDN_STATE;
}
HLE(_ZN2nn3ldn16CloseAccessPointEv) { LDN_TRACE;
    AcquireSRWLockExclusive(&L.lock);
    if (L.state == LS_AP_CREATED) ldn_send(NET_BROADCAST, LM_GONE, L.net + NI_SESSION, 16);
    if (L.state == LS_AP || L.state == LS_AP_CREATED) ldn_state_to(LS_INITIALIZED);
    ReleaseSRWLockExclusive(&L.lock);
    c->x[0] = 0;
}

/* A new network, this player its host (node 0). */
static void create_network(Cpu *c, u64 config, u64 security, u64 secparam, u64 user) {
    AcquireSRWLockExclusive(&L.lock);
    if (L.state != LS_AP || !net_local_ip()) { ReleaseSRWLockExclusive(&L.lock); c->x[0] = R_LDN_STATE; return; }
    u8 *n = L.net;
    memset(n, 0, NI_SIZE);
    memcpy(n + NI_LCID, P(void, config), 8);
    memcpy(n + NI_SCENE, P(void, config + 0xA), 2);
    if (secparam) { memcpy(n + NI_SECPARAM, P(void, secparam), 16); memcpy(n + NI_SESSION, P(void, secparam + 16), 16); }
    else for (int i = 0; i < 32; i++) { unsigned int r; rand_s(&r); n[(i < 16 ? NI_SECPARAM : NI_SESSION - 16) + i] = (u8)r; }
    u32 me = net_local_ip();
    make_mac(n + NI_BSSID, me);
    static const char HEX[] = "0123456789abcdef";
    n[NI_SSID_LEN] = 32;
    for (int i = 0; i < 32; i++) n[NI_SSID + i] = (u8)HEX[n[NI_SESSION + i / 2] >> (i & 1 ? 0 : 4) & 15];
    i16 channel = (i16)ld16(config + 0x10);
    put_u16(n + NI_CHANNEL, (u16)(channel > 0 ? channel : 6));
    n[NI_LINK] = 3;
    n[NI_TYPE] = 2;   /* LDN */
    put_u16(n + NI_SECMODE, ld16(security));
    n[NI_NODE_MAX] = ld8(config + 0x12) && ld8(config + 0x12) <= 8 ? ld8(config + 0x12) : 8;
    n[NI_NODE_COUNT] = 1;
    u8 *host = n + NODE(0);
    put_u32(host, me);
    make_mac(host + 4, me);
    host[0xA] = 0;
    host[0xB] = 1;
    memcpy(host + 0xC, P(void, user), 32);
    memcpy(host + 0x2E, P(void, config + 0x14), 2);
    ldn_state_to(LS_AP_CREATED);
    ldn_send(NET_BROADCAST, LM_ADVERTISE, n, NI_SIZE);
    if (net_log()) fprintf(stderr, "[net] ldn: network created by %.32s (%u players at most)\n", (const char *)host + 0xC, n[NI_NODE_MAX]);
    ReleaseSRWLockExclusive(&L.lock);
    c->x[0] = 0;
}
HLE(_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE) { LDN_TRACE;
    create_network(c, c->x[0], c->x[1], 0, c->x[2]);
}
HLE(_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE) { LDN_TRACE;
    create_network(c, c->x[0], c->x[1], c->x[2], c->x[3]);
}
HLE(_ZN2nn3ldn14DestroyNetworkEv) { LDN_TRACE;
    AcquireSRWLockExclusive(&L.lock);
    if (L.state == LS_AP_CREATED) { ldn_send(NET_BROADCAST, LM_GONE, L.net + NI_SESSION, 16); ldn_state_to(LS_AP); }
    ReleaseSRWLockExclusive(&L.lock);
    c->x[0] = 0;
}

/* The networks seen lately that match a scan filter (flags: 1 local communication id,
 * 2 session id, 4 network type, 0x10 SSID, 0x20 scene id). */
static void scan(Cpu *c, u64 out, u64 count_p, i32 room, u64 filter) {
    u32 flags = ld32(filter + 0x5C);
    if (net_lobby()) {   /* online: the lobby lists the rooms - wait a moment for its list, as a real scan listens a while */
        AcquireSRWLockExclusive(&L.lock);
        u32 seq = ++L.list_asked;
        ReleaseSRWLockExclusive(&L.lock);
        net_lobby_list(seq);
        u64 until = now_ms_() + LIST_WAIT_MS, now;
        AcquireSRWLockExclusive(&L.lock);
        while (L.list_done != seq && (now = now_ms_()) < until) SleepConditionVariableSRW(&L.changed, &L.lock, (DWORD)(until - now), 0);
        if (L.list_done != seq && net_log()) fprintf(stderr, "[net] ldn: no room list from the lobby in time\n");
        ReleaseSRWLockExclusive(&L.lock);
    }
    u64 t = now_ms_();
    i32 found = 0;
    AcquireSRWLockShared(&L.lock);
    for (int i = 0; i < SEEN_NETWORKS && found < room; i++) {
        const u8 *n = L.seen[i].info;
        if (!L.seen[i].at || t - L.seen[i].at > 3500) continue;
        if (L.state == LS_AP_CREATED && !memcmp(n + NI_SESSION, L.net + NI_SESSION, 16)) continue;
        if ((flags & 1) && memcmp(n + NI_LCID, P(void, filter), 8)) continue;
        if ((flags & 2) && memcmp(n + NI_SESSION, P(void, filter + 0x10), 16)) continue;
        if ((flags & 0x10) && (n[NI_SSID_LEN] != ld8(filter + 0x2A) || memcmp(n + NI_SSID, P(void, filter + 0x2B), n[NI_SSID_LEN]))) continue;
        if ((flags & 0x20) && memcmp(n + NI_SCENE, P(void, filter + 0xA), 2)) continue;
        memcpy(P(void, out + (u64)NI_SIZE * found), n, NI_SIZE);
        found++;
    }
    ReleaseSRWLockShared(&L.lock);
    st32(count_p, (u32)found);
    if (net_log() > 1 || (net_log() && found)) fprintf(stderr, "[net] ldn: scan found %d network%s\n", found, found == 1 ? "" : "s");
    c->x[0] = 0;
}
HLE(_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi) { LDN_TRACE; scan(c, c->x[0], c->x[1], (i32)c->x[2], c->x[3]); }
HLE(_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi) { LDN_TRACE; scan(c, c->x[0], c->x[1], (i32)c->x[2], c->x[3]); }

/* Join the network with this session id: ask its host (from its advertisement), wait for the
 * answer - a few tries, a few seconds. */
static void join(Cpu *c, const u8 *session, u64 user, u16 version) {
    AcquireSRWLockExclusive(&L.lock);
    u32 host = 0;
    for (int i = 0; i < SEEN_NETWORKS && !host; i++)
        if (L.seen[i].at && !memcmp(L.seen[i].info + NI_SESSION, session, 16)) host = get_u32(L.seen[i].info + NODE(0));
    if (L.state != LS_STATION || !host) {
        ReleaseSRWLockExclusive(&L.lock);
        if (net_log()) fprintf(stderr, "[net] ldn: cannot join (%s)\n", host ? "not a station" : "network not seen");
        c->x[0] = host ? R_LDN_STATE : R_LDN_CONNECT;
        return;
    }
    u8 ask[52] = {0};
    memcpy(ask, session, 16);
    memcpy(ask + 16, P(void, user), 32);
    memcpy(ask + 48, &version, 2);
    L.join_answer = 0;
    L.join_host = host;
    memcpy(L.join_session, session, 16);
    for (int tries = 0; tries < 5 && !L.join_answer; tries++) {
        ldn_send(host, LM_JOIN, ask, sizeof ask);
        u64 until = now_ms_() + 1000;
        while (!L.join_answer && now_ms_() < until) SleepConditionVariableSRW(&L.changed, &L.lock, (DWORD)(until - now_ms_()), 0);
    }
    int accepted = L.join_answer == 1;
    if (accepted) {
        L.host_heard = now_ms_();
        L.reason = DR_NONE;
        ldn_state_to(LS_CONNECTED);
    }
    if (net_log()) fprintf(stderr, "[net] ldn: joining %.32s's network: %s\n", (const char *)L.net + NODE(0) + 0xC,
                           accepted ? "joined" : L.join_answer ? "refused" : "no answer");
    L.join_answer = 0;
    L.join_host = 0;
    ReleaseSRWLockExclusive(&L.lock);
    c->x[0] = accepted ? 0 : R_LDN_CONNECT;
}
HLE(_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE) { LDN_TRACE;
    join(c, P(u8, c->x[0] + NI_SESSION), c->x[2], (u16)c->x[3]);
}
HLE(_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE) { LDN_TRACE;
    join(c, P(u8, c->x[2] + 16), c->x[3], (u16)c->x[4]);
}
HLE(_ZN2nn3ldn10DisconnectEv) { LDN_TRACE;
    AcquireSRWLockExclusive(&L.lock);
    if (L.state == LS_CONNECTED) {
        ldn_send(get_u32(L.net + NODE(0)), LM_LEAVE, L.net + NI_SESSION, 16);
        L.reason = DR_USER;
        ldn_state_to(LS_STATION);
    }
    ReleaseSRWLockExclusive(&L.lock);
    c->x[0] = 0;
}

static int has_network(void) { return L.state == LS_AP_CREATED || L.state == LS_CONNECTED; }
int ldn_in_network(u32 ip) {
    AcquireSRWLockShared(&L.lock);
    int in = has_network() && node_of(ip) >= 0;
    ReleaseSRWLockShared(&L.lock);
    return in;
}
HLE(_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE) { LDN_TRACE;
    AcquireSRWLockShared(&L.lock);
    int ok = has_network();
    if (ok) memcpy(P(void, c->x[0]), L.net, NI_SIZE);
    ReleaseSRWLockShared(&L.lock);
    c->x[0] = ok ? 0 : R_LDN_STATE;
}
HLE(_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE) { LDN_TRACE;   /* intent id, channel, node maximum, version */
    AcquireSRWLockShared(&L.lock);
    int ok = has_network();
    if (ok) {
        u8 *o = P(u8, c->x[0]);
        memset(o, 0, 0x20);
        memcpy(o, L.net + NI_LCID, 8);
        memcpy(o + 0xA, L.net + NI_SCENE, 2);
        memcpy(o + 0x10, L.net + NI_CHANNEL, 2);
        o[0x12] = L.net[NI_NODE_MAX];
        memcpy(o + 0x14, L.net + NODE(0) + 0x2E, 2);
    }
    ReleaseSRWLockShared(&L.lock);
    c->x[0] = ok ? 0 : R_LDN_STATE;
}
HLE(_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE) { LDN_TRACE;
    if (!net_local_ip()) { c->x[0] = R_LDN_STATE; return; }
    st32(c->x[0], net_local_ip());
    st32(c->x[1], NET_MASK);
    c->x[0] = 0;
}
HLE(_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE) { LDN_TRACE;   /* data[16], session id[16] */
    AcquireSRWLockShared(&L.lock);
    int ok = has_network();
    if (ok) { memcpy(P(void, c->x[0]), L.net + NI_SECPARAM, 16); memcpy(P(void, c->x[0] + 16), L.net + NI_SESSION, 16); }
    ReleaseSRWLockShared(&L.lock);
    c->x[0] = ok ? 0 : R_LDN_STATE;
}
HLE(_ZN2nn3ldn19GetDisconnectReasonEv) { LDN_TRACE; c->x[0] = (u32)L.reason; }
HLE(_ZN2nn3ldn16SetAdvertiseDataEPKvm) { LDN_TRACE;
    u64 size = c->x[1] < 0x180 ? c->x[1] : 0x180;
    AcquireSRWLockExclusive(&L.lock);
    put_u16(L.net + NI_ADV_SIZE, (u16)size);
    memset(L.net + NI_ADV, 0, 0x180);
    if (size) memcpy(L.net + NI_ADV, P(void, c->x[0]), size);
    if (L.state == LS_AP_CREATED) ldn_send(NET_BROADCAST, LM_ADVERTISE, L.net, NI_SIZE);
    ReleaseSRWLockExclusive(&L.lock);
    c->x[0] = 0;
}
HLE(_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE) { LDN_TRACE;
    AcquireSRWLockExclusive(&L.lock);
    L.net[NI_POLICY] = (u8)c->x[0];
    ReleaseSRWLockExclusive(&L.lock);
    c->x[0] = 0;
}
HLE(_ZN2nn3ldn6RejectENS0_11Ipv4AddressE) { LDN_TRACE;   /* (host) send a station away */
    LDN_TRACE;
    u32 ip = (u32)c->x[0];
    AcquireSRWLockExclusive(&L.lock);
    int node = L.state == LS_AP_CREATED ? node_of(ip) : -1;
    if (node > 0) { ldn_send(ip, LM_KICK, L.net + NI_SESSION, 16); drop_node(node); }
    ReleaseSRWLockExclusive(&L.lock);
    c->x[0] = 0;
}
HLE(_ZN2nn3ldn32SetWirelessControllerRestrictionENS0_29WirelessControllerRestrictionE) { LDN_TRACE; c->x[0] = 0; }

/* ---- nn::socket: datagram sockets on the virtual LAN -------------------------------- */
typedef struct Datagram { struct Datagram *next; u32 src_ip, len; u16 src_port; u8 data[]; } Datagram;
typedef struct VSock {
    int open, type, flags, nonblock, broadcast, reuse, shut_rd, shut_wr;
    u32 ip, peer_ip;          /* bound address (0: any) and connected peer, host order */
    u16 port, peer_port;      /* bound port (0: not bound yet), connected peer's (0: none) */
    u32 rcvtimeo_ms, rcvbuf, sndbuf;
    Datagram *head, *tail;
    u32 queued, queued_bytes;
} VSock;
#define VSOCKS 128
#define FD_BASE 3   /* (fds start above the standard streams' numbers) */
#define QUEUE_BYTES (1u << 20)
static VSock socks[VSOCKS];
static SRWLOCK lock = SRWLOCK_INIT;
static CONDITION_VARIABLE activity = CONDITION_VARIABLE_INIT;   /* a datagram arrived, a socket closed */
static u16 next_ephemeral = 49152;

static VSock *sock_of(i32 fd) {
    if (fd < FD_BASE || fd >= FD_BASE + VSOCKS || !socks[fd - FD_BASE].open) return NULL;
    return &socks[fd - FD_BASE];
}

/* Stream sockets are the Online mode's (runtime/hle_online.c: NEX's TCP to the game server).
 * (It wakes poll() and blocking calls through net_wake: the lock first, so a waiter is either
 * still looking - and sees the change - or already asleep.) */
static int is_stream(i32 fd) {
    AcquireSRWLockShared(&lock);
    VSock *s = sock_of(fd);
    int r = s && s->type == SOCK_STREAM_;
    ReleaseSRWLockShared(&lock);
    return r;
}
void net_wake(void) {
    AcquireSRWLockExclusive(&lock);
    ReleaseSRWLockExclusive(&lock);
    WakeAllConditionVariable(&activity);
}

static void set_errno(Cpu *c, int e) {
    u64 p = rt_symbol("__errno_location") ? rt_callsym(c, "__errno_location", 0) : 0;
    if (p) st32(p, (u32)e);
}
static int get_errno(Cpu *c) {
    u64 p = rt_symbol("__errno_location") ? rt_callsym(c, "__errno_location", 0) : 0;
    return p ? (int)ld32(p) : 0;
}
static void fail(Cpu *c, int e) { set_errno(c, e); c->x[0] = (u64)(i64)-1; }

static const char *ip_text(u32 ip, char *buf) {
    snprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
    return buf;
}

/* sockaddr_in: { u8 len; u8 family; u16 port; u32 address; u8 zero[8] }, network order */
static int read_addr(u64 a, u32 len, u32 *ip, u16 *port) {
    if (!a || len < 8 || ld8(a + 1) != AF_INET_) return 0;
    *port = _byteswap_ushort(ld16(a + 2));
    *ip = _byteswap_ulong(ld32(a + 4));
    return 1;
}
static void write_addr(u64 a, u64 alen_p, u32 ip, u16 port) {
    if (!a) return;
    u8 b[16] = {16, AF_INET_};
    u16 p = _byteswap_ushort(port);
    u32 i = _byteswap_ulong(ip);
    memcpy(b + 2, &p, 2);
    memcpy(b + 4, &i, 4);
    u32 cap = alen_p ? ld32(alen_p) : 16;
    memcpy(P(void, a), b, cap < 16 ? cap : 16);
    if (alen_p) st32(alen_p, 16);
}

static int port_taken(const VSock *self, u16 port) {
    for (u32 i = 0; i < VSOCKS; i++) {
        const VSock *s = &socks[i];
        if (s != self && s->open && s->type == self->type && s->port == port && !(s->reuse && self->reuse)) return 1;
    }
    return 0;
}
static u16 ephemeral(const VSock *self) {
    for (u32 tries = 0; tries < 16384; tries++) {
        u16 p = next_ephemeral++;
        if (next_ephemeral < 49152) next_ephemeral = 49152;
        if (!port_taken(self, p)) return p;
    }
    return 0;
}

static void drop_queue(VSock *s) {
    while (s->head) { Datagram *d = s->head; s->head = d->next; free(d); }
    s->tail = NULL;
    s->queued = s->queued_bytes = 0;
}

void vsock_deliver(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len) {
    if (dst_port == LDN_PORT) {   /* (local wireless's own messages: from local wireless alone) */
        if (src_port == LDN_PORT) ldn_receive(src_ip, data, len);
        return;
    }
    int broadcast = dst_ip == ANY_BROADCAST || dst_ip == NET_BROADCAST, delivered = 0;
    AcquireSRWLockExclusive(&lock);
    for (u32 i = 0; i < VSOCKS; i++) {
        VSock *s = &socks[i];
        if (!s->open || s->type != SOCK_DGRAM_ || s->port != dst_port || s->shut_rd) continue;
        if (!broadcast && s->ip && s->ip != dst_ip && s->ip != LOOPBACK) continue;   /* bound to another address */
        if (s->peer_port && (s->peer_ip != src_ip || s->peer_port != src_port)) continue;   /* connected elsewhere */
        if (s->queued >= 1024 || s->queued_bytes + len > QUEUE_BYTES) continue;   /* (full: dropped, as a LAN would) */
        Datagram *d = malloc(sizeof *d + len);
        if (!d) continue;
        d->next = NULL; d->src_ip = src_ip; d->src_port = src_port; d->len = len;
        memcpy(d->data, data, len);
        if (s->tail) s->tail->next = d; else s->head = d;
        s->tail = d;
        s->queued++;
        s->queued_bytes += len;
        delivered = 1;
        if (!broadcast && !s->reuse) break;
    }
    ReleaseSRWLockExclusive(&lock);
    if (delivered) WakeAllConditionVariable(&activity);
}

static void do_socket(Cpu *c, i32 domain, i32 type, i32 proto) {
    i32 base = type & 0xFFFF;
    if (domain != AF_INET_) { if (net_log()) fprintf(stderr, "[net] socket(%d, %#x, %d): family unsupported\n", domain, type, proto); fail(c, E_AFNOSUPPORT); return; }
    if (base != SOCK_STREAM_ && base != SOCK_DGRAM_) { fail(c, E_PROTONOSUPPORT); return; }
    AcquireSRWLockExclusive(&lock);
    i32 fd = -1;
    for (u32 i = 0; i < VSOCKS && fd < 0; i++)
        if (!socks[i].open) {
            memset(&socks[i], 0, sizeof socks[i]);
            socks[i].open = 1;
            socks[i].type = base;
            socks[i].nonblock = (type & (SOCK_NONBLOCK_ | O_NONBLOCK_)) != 0;
            socks[i].rcvbuf = socks[i].sndbuf = 0x10000;
            fd = (i32)i + FD_BASE;
        }
    ReleaseSRWLockExclusive(&lock);
    if (net_log()) fprintf(stderr, "[net] socket(%d, %#x, %d) = %d%s\n", domain, type, proto, fd, base == SOCK_STREAM_ ? " (stream)" : "");
    if (fd < 0) { fail(c, E_MFILE); return; }
    c->x[0] = (u64)(i64)fd;
}

static void do_bind(Cpu *c, i32 fd, u64 addr, u32 len) {
    u32 ip;
    u16 port;
    char t[16];
    AcquireSRWLockExclusive(&lock);
    VSock *s = sock_of(fd);
    int err = !s ? E_BADF : !read_addr(addr, len, &ip, &port) ? E_INVAL : 0;
    if (!err && ip && ip != LOOPBACK && ip != net_local_ip() && ip != ANY_BROADCAST && ip != NET_BROADCAST) err = E_ADDRNOTAVAIL;
    if (!err && port && port_taken(s, port)) err = E_ADDRINUSE;
    if (!err && !port && !(port = ephemeral(s))) err = E_ADDRINUSE;
    if (!err) { s->ip = ip; s->port = port; }
    ReleaseSRWLockExclusive(&lock);
    if (net_log()) fprintf(stderr, "[net] bind(%d, %s:%u) = %s%d\n", fd, ip_text(err ? 0 : ip, t), port, err ? "error " : "", err);
    if (err) fail(c, err); else c->x[0] = 0;
}

static void do_connect(Cpu *c, i32 fd, u64 addr, u32 len) {
    u32 ip;
    u16 port;
    char t[16];
    AcquireSRWLockExclusive(&lock);
    VSock *s = sock_of(fd);
    int err = !s ? E_BADF : !read_addr(addr, len, &ip, &port) ? E_INVAL : 0;
    int stream = s && s->type == SOCK_STREAM_, nonblock = s && s->nonblock;
    if (!err) {
        if (!s->port) s->port = ephemeral(s);
        s->peer_ip = ip;
        s->peer_port = port;
    }
    u16 my_port = s ? s->port : 0;
    ReleaseSRWLockExclusive(&lock);
    if (!err && stream) err = online_stream_connect(fd, ip, port, net_local_ip(), my_port, nonblock);
    if (net_log()) fprintf(stderr, "[net] connect(%d, %s:%u) = %d%s\n", fd, ip_text(ip, t), port, err, stream ? " (stream: Online)" : "");
    if (err) fail(c, err); else c->x[0] = 0;
}

static void do_sendto(Cpu *c, i32 fd, u64 buf, u64 len, i32 flags, u64 addr, u32 alen) {
    u32 dst = 0;
    u16 dport = 0;
    char t[16];
    if (is_stream(fd)) {
        AcquireSRWLockShared(&lock);
        VSock *ss = sock_of(fd);
        int nonblock = ss && (ss->nonblock || (flags & MSG_DONTWAIT_));
        ReleaseSRWLockShared(&lock);
        i64 r = online_stream_send(fd, P(void, buf), len, nonblock);
        if (r < 0) fail(c, (int)-r); else c->x[0] = (u64)r;
        return;
    }
    AcquireSRWLockExclusive(&lock);
    VSock *s = sock_of(fd);
    int err = 0;
    if (!s) err = E_BADF;
    else if (s->type != SOCK_DGRAM_) err = E_NOTCONN;
    else if (s->shut_wr) err = E_NOTCONN;
    else if (addr && alen) { if (!read_addr(addr, alen, &dst, &dport)) err = E_INVAL; }
    else if (s->peer_port) { dst = s->peer_ip; dport = s->peer_port; }
    else err = E_DESTADDRREQ;
    if (!err && len > 65507) err = E_MSGSIZE;
    if (!err && !s->port && !(s->port = ephemeral(s))) err = E_ADDRINUSE;
    u16 sport = s && !err ? s->port : 0;
    ReleaseSRWLockExclusive(&lock);
    u32 me = net_local_ip();
    int broadcast = dst == ANY_BROADCAST || dst == NET_BROADCAST;
    if (!err && !broadcast && dst != LOOPBACK && dst != me && ((dst & NET_MASK) != NET_SUBNET || !me)) err = E_NETUNREACH;
    if (net_log() > 1 || (net_log() && err))
        fprintf(stderr, "[net] sendto(%d, %llu bytes, %s:%u) from port %u%s%d\n", fd, len, ip_text(dst, t), dport, sport, err ? ": error " : " ", err);
    if (err) { fail(c, err); return; }
    const void *data = P(void, buf);
    if (dst == LOOPBACK) vsock_deliver(LOOPBACK, sport, LOOPBACK, dport, data, (u32)len);
    else {
        if (dst == me || broadcast) vsock_deliver(me, sport, dst, dport, data, (u32)len);   /* (a broadcast reaches this player's sockets too) */
        if (dst != me) net_send(me, sport, broadcast ? NET_BROADCAST : dst, dport, data, (u32)len);
    }
    c->x[0] = len;
}

/* Wait for the activity condition until the deadline (ms since boot; ~0: for ever). Returns
 * 0 once it has passed. Called holding the lock. */
static int wait_activity(u64 deadline) {
    DWORD ms = INFINITE;
    if (deadline != ~0ull) {
        u64 now = rt_now_ns() / 1000000;
        if (now >= deadline) return 0;
        ms = (DWORD)(deadline - now);
    }
    SleepConditionVariableSRW(&activity, &lock, ms, 0);
    return 1;
}

static void do_recvfrom(Cpu *c, i32 fd, u64 buf, u64 len, i32 flags, u64 addr, u64 alen_p) {
    if (is_stream(fd)) {
        AcquireSRWLockShared(&lock);
        VSock *ss = sock_of(fd);
        int nonblock = ss && (ss->nonblock || (flags & MSG_DONTWAIT_));
        u32 ip = ss ? ss->peer_ip : 0;
        u16 port = ss ? ss->peer_port : 0;
        ReleaseSRWLockShared(&lock);
        i64 r = online_stream_recv(fd, P(void, buf), len, (flags & MSG_PEEK_) != 0, nonblock);
        if (r < 0) { fail(c, (int)-r); return; }
        write_addr(addr, alen_p, ip, port);
        c->x[0] = (u64)r;
        return;
    }
    AcquireSRWLockExclusive(&lock);
    VSock *s = sock_of(fd);
    u64 deadline = s && s->rcvtimeo_ms ? rt_now_ns() / 1000000 + s->rcvtimeo_ms : ~0ull;
    int err = 0;
    for (;;) {
        s = sock_of(fd);
        if (!s) { err = E_BADF; break; }
        if (s->head) break;
        if (s->type != SOCK_DGRAM_ && !s->peer_port && !s->port) { err = E_NOTCONN; break; }
        if (s->shut_rd) { ReleaseSRWLockExclusive(&lock); c->x[0] = 0; return; }
        if (s->nonblock || (flags & MSG_DONTWAIT_)) { err = E_AGAIN; break; }
        if (!wait_activity(deadline)) { err = E_AGAIN; break; }
    }
    if (err) { ReleaseSRWLockExclusive(&lock); fail(c, err); return; }
    Datagram *d = s->head;
    u32 n = d->len < len ? d->len : (u32)len;
    memcpy(P(void, buf), d->data, n);
    u32 ip = d->src_ip;
    u16 port = d->src_port;
    if (!(flags & MSG_PEEK_)) {
        s->head = d->next;
        if (!s->head) s->tail = NULL;
        s->queued--;
        s->queued_bytes -= d->len;
        free(d);
    }
    ReleaseSRWLockExclusive(&lock);
    write_addr(addr, alen_p, ip, port);
    if (net_log() > 1) { char t[16]; fprintf(stderr, "[net] recvfrom(%d) = %u bytes from %s:%u\n", fd, n, ip_text(ip, t), port); }
    c->x[0] = n;
}

static void do_poll(Cpu *c, u64 fds, u64 nfds, i32 timeout) {
    u64 deadline = timeout < 0 ? ~0ull : rt_now_ns() / 1000000 + (u64)timeout;
    u32 ready;
    AcquireSRWLockExclusive(&lock);
    for (;;) {
        ready = 0;
        for (u64 i = 0; i < nfds; i++) {
            u64 p = fds + 8 * i;
            i32 fd = (i32)ld32(p);
            u16 events = ld16(p + 4), revents = 0;
            if (fd < 0) { st16(p + 6, 0); continue; }
            VSock *s = sock_of(fd);
            if (!s) revents = POLLNVAL_;
            else if (s->type == SOCK_STREAM_) revents = online_stream_revents(fd, events);
            else {
                if (s->head) revents |= events & (POLLIN_ | POLLRDNORM_);
                if (s->type == SOCK_DGRAM_ && !s->shut_wr) revents |= events & POLLOUT_;
                if (s->shut_rd && s->type == SOCK_STREAM_) revents |= POLLHUP_;
            }
            st16(p + 6, revents);
            ready += revents != 0;
        }
        if (ready || timeout == 0 || !wait_activity(deadline)) break;
    }
    ReleaseSRWLockExclusive(&lock);
    if (net_log() > 2) fprintf(stderr, "[net] poll(%llu fds, %d ms) = %u\n", nfds, timeout, ready);
    c->x[0] = ready;
}

static void do_fcntl(Cpu *c, i32 fd, i32 cmd, i32 arg) {
    AcquireSRWLockExclusive(&lock);
    VSock *s = sock_of(fd);
    i32 r = 0;
    if (s && cmd == F_GETFL_) r = s->flags | (s->nonblock ? O_NONBLOCK_ : 0);
    else if (s && cmd == F_SETFL_) { s->flags = arg; s->nonblock = (arg & (O_NONBLOCK_ | O_NONBLOCK_BSD)) != 0; }
    ReleaseSRWLockExclusive(&lock);
    if (net_log()) fprintf(stderr, "[net] fcntl(%d, %d, %#x) = %#x\n", fd, cmd, arg, r);
    if (!s) fail(c, E_BADF); else c->x[0] = (u64)(i64)r;
}

static void do_setsockopt(Cpu *c, i32 fd, i32 level, i32 opt, u64 val, u32 len) {
    u32 v = val && len >= 4 ? ld32(val) : 0;
    AcquireSRWLockExclusive(&lock);
    VSock *s = sock_of(fd);
    if (s && level == SOL_SOCKET_) {
        if (opt == SO_REUSEADDR_ || opt == SO_REUSEPORT_) s->reuse = v != 0;
        else if (opt == SO_BROADCAST_) s->broadcast = v != 0;
        else if (opt == SO_RCVBUF_) s->rcvbuf = v;
        else if (opt == SO_SNDBUF_) s->sndbuf = v;
        else if (opt == SO_RCVTIMEO_ && val && len >= 16) {   /* struct timeval { s64 sec; s64 usec } */
            i64 sec = (i64)ld64(val), usec = (i64)ld64(val + 8);
            s->rcvtimeo_ms = (u32)(sec * 1000 + usec / 1000);
            if (!s->rcvtimeo_ms && (sec || usec)) s->rcvtimeo_ms = 1;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    if (net_log()) fprintf(stderr, "[net] setsockopt(%d, %#x, %#x, %u (%u bytes))\n", fd, level, opt, v, len);
    if (!s) fail(c, E_BADF); else c->x[0] = 0;
}

static void do_getsockopt(Cpu *c, i32 fd, i32 level, i32 opt, u64 val, u64 len_p) {
    AcquireSRWLockExclusive(&lock);
    VSock *s = sock_of(fd);
    u32 v = 0;
    int err = !s ? E_BADF : 0;
    if (s && level == SOL_SOCKET_) {
        if (opt == SO_ERROR_) v = s->type == SOCK_STREAM_ ? (u32)online_stream_error(fd) : 0;
        else if (opt == SO_TYPE_) v = (u32)s->type;
        else if (opt == SO_RCVBUF_) v = s->rcvbuf;
        else if (opt == SO_SNDBUF_) v = s->sndbuf;
        else if (opt == SO_REUSEADDR_ || opt == SO_REUSEPORT_) v = (u32)s->reuse;
        else if (opt == SO_BROADCAST_) v = (u32)s->broadcast;
    } else if (s && level != 0 && level != 6) err = E_NOPROTOOPT;
    ReleaseSRWLockExclusive(&lock);
    if (net_log()) fprintf(stderr, "[net] getsockopt(%d, %#x, %#x) = %u\n", fd, level, opt, v);
    if (err) { fail(c, err); return; }
    if (val && len_p && ld32(len_p) >= 4) { st32(val, v); st32(len_p, 4); }
    c->x[0] = 0;
}

static void do_getsockname(Cpu *c, i32 fd, u64 addr, u64 alen_p, int peer) {
    if (net_log() > 1) fprintf(stderr, "[net] get%sname(%d)\n", peer ? "peer" : "sock", fd);
    AcquireSRWLockShared(&lock);
    VSock *s = sock_of(fd);
    u32 ip = s ? (peer ? s->peer_ip : s->ip) : 0;
    u16 port = s ? (peer ? s->peer_port : s->port) : 0;
    ReleaseSRWLockShared(&lock);
    if (!s) { fail(c, E_BADF); return; }
    if (peer && !port) { fail(c, E_NOTCONN); return; }
    write_addr(addr, alen_p, ip, port);
    c->x[0] = 0;
}

static void do_shutdown(Cpu *c, i32 fd, i32 how) {
    AcquireSRWLockExclusive(&lock);
    VSock *s = sock_of(fd);
    if (s) { if (how != 1) s->shut_rd = 1; if (how != 0) s->shut_wr = 1; }
    ReleaseSRWLockExclusive(&lock);
    WakeAllConditionVariable(&activity);
    if (!s) fail(c, E_BADF); else c->x[0] = 0;
}

static void do_close(Cpu *c, i32 fd) {
    if (is_stream(fd)) online_stream_close(fd);
    AcquireSRWLockExclusive(&lock);
    VSock *s = sock_of(fd);
    if (s) { drop_queue(s); memset(s, 0, sizeof *s); }
    ReleaseSRWLockExclusive(&lock);
    WakeAllConditionVariable(&activity);
    if (net_log()) fprintf(stderr, "[net] close(%d)%s\n", fd, s ? "" : ": not a socket");
    if (!s) fail(c, E_BADF); else c->x[0] = 0;
}

static void do_listen(Cpu *c, i32 fd) {
    VSock *s = sock_of(fd);
    if (net_log()) fprintf(stderr, "[net] listen(%d) (streams: nothing will connect)\n", fd);
    if (!s) fail(c, E_BADF); else if (s->type != SOCK_STREAM_) fail(c, E_OPNOTSUPP); else c->x[0] = 0;
}

static void do_accept(Cpu *c, i32 fd) {   /* no stream ever connects: wait until the socket goes */
    AcquireSRWLockExclusive(&lock);
    VSock *s = sock_of(fd);
    int err = !s ? E_BADF : s->nonblock ? E_AGAIN : 0;
    while (!err) {
        wait_activity(~0ull);
        if (!(s = sock_of(fd)) || s->shut_rd) err = E_BADF;
    }
    ReleaseSRWLockExclusive(&lock);
    fail(c, err);
}

/* the C++ API (both overload sets: plain ints and the nn::socket enum/SockAddr types) */
HLE(_ZN2nn6socket6SocketEiii) { do_socket(c, (i32)c->x[0], (i32)c->x[1], (i32)c->x[2]); }
HLE(_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE) { do_socket(c, (i32)c->x[0], (i32)c->x[1], (i32)c->x[2]); }
HLE(_ZN2nn6socket4BindEiPK8sockaddrj) { do_bind(c, (i32)c->x[0], c->x[1], (u32)c->x[2]); }
HLE(_ZN2nn6socket4BindEiPKNS0_8SockAddrEj) { do_bind(c, (i32)c->x[0], c->x[1], (u32)c->x[2]); }
HLE(_ZN2nn6socket7ConnectEiPK8sockaddrj) { do_connect(c, (i32)c->x[0], c->x[1], (u32)c->x[2]); }
HLE(_ZN2nn6socket6ListenEii) { do_listen(c, (i32)c->x[0]); }
HLE(_ZN2nn6socket6AcceptEiP8sockaddrPj) { do_accept(c, (i32)c->x[0]); }
HLE(_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj) { do_sendto(c, (i32)c->x[0], c->x[1], c->x[2], (i32)c->x[3], c->x[4], (u32)c->x[5]); }
HLE(_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj) { do_sendto(c, (i32)c->x[0], c->x[1], c->x[2], (i32)c->x[3], c->x[4], (u32)c->x[5]); }
HLE(_ZN2nn6socket4SendEiPKvmi) { do_sendto(c, (i32)c->x[0], c->x[1], c->x[2], (i32)c->x[3], 0, 0); }
HLE(_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj) { do_recvfrom(c, (i32)c->x[0], c->x[1], c->x[2], (i32)c->x[3], c->x[4], c->x[5]); }
HLE(_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj) { do_recvfrom(c, (i32)c->x[0], c->x[1], c->x[2], (i32)c->x[3], c->x[4], c->x[5]); }
HLE(_ZN2nn6socket4RecvEiPvmi) { do_recvfrom(c, (i32)c->x[0], c->x[1], c->x[2], (i32)c->x[3], 0, 0); }
HLE(_ZN2nn6socket4PollEP6pollfdmi) { do_poll(c, c->x[0], c->x[1], (i32)c->x[2]); }
HLE(_ZN2nn6socket5FcntlEiiz) { do_fcntl(c, (i32)c->x[0], (i32)c->x[1], (i32)c->x[2]); }
HLE(_ZN2nn6socket10SetSockOptEiiiPKvj) { do_setsockopt(c, (i32)c->x[0], (i32)c->x[1], (i32)c->x[2], c->x[3], (u32)c->x[4]); }
HLE(_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj) { do_setsockopt(c, (i32)c->x[0], (i32)c->x[1], (i32)c->x[2], c->x[3], (u32)c->x[4]); }
HLE(_ZN2nn6socket10GetSockOptEiiiPvPj) { do_getsockopt(c, (i32)c->x[0], (i32)c->x[1], (i32)c->x[2], c->x[3], c->x[4]); }
HLE(_ZN2nn6socket11GetSockNameEiP8sockaddrPj) { do_getsockname(c, (i32)c->x[0], c->x[1], c->x[2], 0); }
HLE(_ZN2nn6socket8ShutdownEii) { do_shutdown(c, (i32)c->x[0], (i32)c->x[1]); }
HLE(_ZN2nn6socket5CloseEi) { do_close(c, (i32)c->x[0]); }
HLE(_ZN2nn6socket12GetLastErrnoEv) { c->x[0] = (u32)get_errno(c); }

/* the C API */
HLE(nnsocketSocket) { do_socket(c, (i32)c->x[0], (i32)c->x[1], (i32)c->x[2]); }
HLE(nnsocketBind) { do_bind(c, (i32)c->x[0], c->x[1], (u32)c->x[2]); }
HLE(nnsocketConnect) { do_connect(c, (i32)c->x[0], c->x[1], (u32)c->x[2]); }
HLE(nnsocketSend) { do_sendto(c, (i32)c->x[0], c->x[1], c->x[2], (i32)c->x[3], 0, 0); }
HLE(nnsocketRecv) { do_recvfrom(c, (i32)c->x[0], c->x[1], c->x[2], (i32)c->x[3], 0, 0); }
HLE(nnsocketPoll) { do_poll(c, c->x[0], c->x[1], (i32)c->x[2]); }
HLE(nnsocketFcntl) { do_fcntl(c, (i32)c->x[0], (i32)c->x[1], (i32)c->x[2]); }
HLE(nnsocketSetSockOpt) { do_setsockopt(c, (i32)c->x[0], (i32)c->x[1], (i32)c->x[2], c->x[3], (u32)c->x[4]); }
HLE(nnsocketGetSockOpt) { do_getsockopt(c, (i32)c->x[0], (i32)c->x[1], (i32)c->x[2], c->x[3], c->x[4]); }
HLE(nnsocketGetSockName) { do_getsockname(c, (i32)c->x[0], c->x[1], c->x[2], 0); }
HLE(nnsocketGetPeerName) { do_getsockname(c, (i32)c->x[0], c->x[1], c->x[2], 1); }
HLE(nnsocketClose) { do_close(c, (i32)c->x[0]); }
HLE(nnsocketGetLastErrno) { c->x[0] = (u32)get_errno(c); }
HLE(nnsocketSetLastErrno) { set_errno(c, (i32)c->x[0]); }

/* set-up, and name resolution - nothing resolves (there are no servers to find) */
HLE(_ZN2nn6socket10InitializeEPvmmi) { c->x[0] = 0; }
HLE(_ZN2nn6socket8FinalizeEv) { c->x[0] = 0; }
HLE(_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_) { st64(c->x[3], 0); c->x[0] = 4; }   /* EAI_FAIL */
HLE(_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_) { st64(c->x[3], 0); c->x[0] = 4; }
HLE(_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE) {
    static u64 text;   /* (published after the copy: another thread may take it and read it) */
    u64 t = __atomic_load_n(&text, __ATOMIC_ACQUIRE);
    if (!t) __atomic_store_n(&text, t = rt_guest_copy("Name resolution failed (no servers)", 36), __ATOMIC_RELEASE);
    c->x[0] = t;
}
HLE(nnsocketGetHostByNameWithOptions) { c->x[0] = 0; }
HLE(nnsocketResolverGetOption) { c->x[0] = 0; }
HLE(nnsocketResolverSetOption) { c->x[0] = 0; }
