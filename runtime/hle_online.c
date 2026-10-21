/* The game's own Online mode (Quickplay, Battle Arenas, ...) against the NEX game server of
 * tools/nex, which runs inside the online lobby (tools/ssbu_online_lobby.py) at the lobby's
 * virtual address, 10.13.0.1.
 *
 * Online logs in with the network service account's id token (hle_env.c) to the game server
 * "https://g23380901-lp1.s.n.srv.nintendo.net/": NEX's PRUDP Lite, over a WebSocket, over TLS.
 * NEX has a second way in that no retail code uses: a direct server address in the NgsFacade
 * (host at +0x240, an nn::nex::String, and port at +0x268). Set, NEX opens the same WebSocket
 * without TLS to that address - a stream socket, which the stream module below carries to the
 * lobby over TCP. Pia's NAT check servers (nncs1, nncs2) resolve to the server too, and Pia's
 * matches run between the players' virtual addresses, as Local Wireless's do.
 *
 * SSBU_ONLINE_SERVER=host:port overrides the server (default 10.13.0.1:60000); "off" leaves
 * the game's own address (and Online fails, as offline). */
#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#define SD_BOTH SHUT_RDWR
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "net.h"
#include "online.h"

#define DEFAULT_HOST "10.13.0.1"
#define DEFAULT_PORT 60000

/* The server to log in to: 0 when online play is off (it needs the online lobby: a private
 * room's relay in the host's game has no game server). */
static int online_server(char *host, size_t n, u16 *port) {
    const char *v = getenv("SSBU_ONLINE_SERVER");
    if (v && !strcmp(v, "off")) return 0;
    if (!v || !*v) {
        if (!net_configured() || !net_relay_address()) return 0;
        snprintf(host, n, "%s", DEFAULT_HOST);
        *port = DEFAULT_PORT;
        return 1;
    }
    const char *colon = strrchr(v, ':');
    size_t len = colon ? (size_t)(colon - v) : strlen(v);
    if (!len || len >= n) return 0;
    memcpy(host, v, len);
    host[len] = 0;
    *port = colon && atoi(colon + 1) > 0 && atoi(colon + 1) < 65536 ? (u16)atoi(colon + 1) : DEFAULT_PORT;
    return 1;
}

/* The server's IPv4 address (host order), 0 if it is not a dotted address. */
static u32 online_ip(void) {
    char host[64];
    u16 port;
    unsigned a, b, d, e;
    if (!online_server(host, sizeof host, &port) || sscanf(host, "%u.%u.%u.%u", &a, &b, &d, &e) != 4) return 0;
    return a << 24 | b << 16 | d << 8 | e;
}

/* Names of Nintendo's servers (the game server, the NAT check servers) are ours while online. */
static int nintendo_name(const char *name) { return name && strstr(name, "nintendo.net") != NULL; }

/* A dotted IPv4 address (host order), 0 if the name is not one. Station URLs carry addresses
 * this way, and the SDK's resolver (GetAddrInfo) does not work here. */
static u32 dotted_ip(const char *name) {
    unsigned a, b, d, e;
    char end;
    if (!name || sscanf(name, "%u.%u.%u.%u%c", &a, &b, &d, &e, &end) != 4 || (a | b | d | e) > 255) return 0;
    return a << 24 | b << 16 | d << 8 | e;
}

/* NEX's login job, the step that works out the game server's address (main+0x249190;
 * the job is x0, its NgsFacade at +360). Give the facade the direct address first. */
/* signature: f81b0ffc a9015ff8 a90257f6 a9034ff4 a9047bfd 910103fd d10d83ff b9417808 aa0003f3 3100051f 54.....0 9100e3e8 9....... b9403be8 b9418269 4b090108 */
HLE_WRAP(7100249190) {
    u64 facade = ld64(c->x[0] + 360);
    char host[64];
    u16 port;
    if (facade && online_server(host, sizeof host, &port)) {
        u64 str = facade + 576;   /* nn::nex::String { vtable; char local[16]; char *data; u64 capacity } */
        u64 data = ld64(str + 24), capacity = ld64(str + 32);
        if (data && capacity > strlen(host)) {
            memcpy(P(char, data), host, strlen(host) + 1);
            st16(facade + 616, port);
            if (net_log()) fprintf(stderr, "[online] NEX login to %s:%u\n", host, port);
        } else if (net_log()) {
            fprintf(stderr, "[online] NEX server address left as the game's: no room for \"%s\"\n", host);
        }
    }
    f_7100249190_orig(c);
}

/* NEX's InetAddress from a name (main+0x1becd0; x0 the address, x1 the name -> bool):
 * the address is at +12, in network order. */
/* signature: d10643ff f900b3fc a9174ff4 a9187bfd 910603fd aa0103f4 aa0003f3 .0.....1 91....21 aa1403e0 9....... 34.....0 .0.....1 91....21 aa1403e0 9....... */
HLE_WRAP(71001becd0) {
    const char *name = c->x[1] ? P(const char, c->x[1]) : NULL;
    int ours = nintendo_name(name);
    u32 ip = ours ? online_ip() : dotted_ip(name);
    if (net_log() && ours) fprintf(stderr, "[online] NEX resolves \"%s\"%s\n", name, ip ? " -> the online server" : "");
    if (ip) {
        st32(c->x[0] + 12, _byteswap_ulong(ip));
        c->x[0] = 1;
        return;
    }
    f_71001becd0_orig(c);
}

/* Pia's name resolution (main+0xba850; x0 the name, x4 a Pia InetAddress, whose IPv4
 * address is at +8 in host order -> bool): the NAT check servers are the online server too.
 * Pia's NAT check needs its two servers at different addresses: nncs2 gets the last address of
 * the server's /16 (10.13.255.254), which the online lobby keeps for it (tools/nex/server.py). */
/* signature: d10183ff f9001bf5 a9044ff4 a9057bfd 910143fd aa0103f4 aa0003f5 910003e0 321c07e2 2a1f03e1 aa0403f3 9....... 321f03e8 910003e2 d10063a3 aa1503e0 */
HLE_WRAP(71000ba850) {
    const char *name = c->x[0] ? P(const char, c->x[0]) : NULL;
    int ours = nintendo_name(name);
    u32 ip = ours ? online_ip() : dotted_ip(name);
    if (ip && ours && !strncmp(name, "nncs2", 5)) ip = (ip & 0xFFFF0000) | 0xFFFE;
    if (net_log() && ours) fprintf(stderr, "[online] Pia resolves \"%s\"%s\n", name, ip ? " -> the online server" : "");
    if (ip) {
        if (c->x[4]) {
            memset(P(u8, c->x[4] + 8), 0, 16);
            st32(c->x[4] + 8, ip);
        }
        c->x[0] = 1;
        return;
    }
    f_71000ba850_orig(c);
}

/* ---- nn::crypto: SHA-1 ------------------------------------------------------------
 * The SDK's uses the CPU's SHA-1 instructions, which the recompiler does not translate; NEX's
 * WebSocket checks the server's Sec-WebSocket-Accept with it. */
static u32 rol(u32 x, int n) { return x << n | x >> (32 - n); }

static void sha1_block(u32 h[5], const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(u8 out[20], const u8 *data, u64 len) {
    u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    u64 i = 0;
    for (; i + 64 <= len; i += 64) sha1_block(h, data + i);
    u8 tail[128] = {0};
    u64 rest = len - i;
    memcpy(tail, data + i, (size_t)rest);
    tail[rest] = 0x80;
    u64 n = rest + 9 <= 64 ? 64 : 128, bits = len * 8;
    for (int k = 0; k < 8; k++) tail[n - 1 - k] = (u8)(bits >> (8 * k));
    sha1_block(h, tail);
    if (n == 128) sha1_block(h, tail + 64);
    for (int k = 0; k < 5; k++) { out[4 * k] = (u8)(h[k] >> 24); out[4 * k + 1] = (u8)(h[k] >> 16); out[4 * k + 2] = (u8)(h[k] >> 8); out[4 * k + 3] = (u8)h[k]; }
}

HLE(_ZN2nn6crypto16GenerateSha1HashEPvmPKvm) {   /* (void *hash, u64 hash size, const void *data, u64 size) */
    u8 h[20];
    sha1(h, P(u8, c->x[2]), c->x[3]);
    memcpy(P(void, c->x[0]), h, c->x[1] < 20 ? (size_t)c->x[1] : 20);
}

/* ---- NEX's stream sockets ---------------------------------------------------------
 * A stream socket connected to the online server is a TCP connection to the lobby
 * (SSBU_NET_RELAY's host, same port number as its UDP), which starts with "SSBUNEX1", this
 * player's virtual address and port and the server port asked for (big-endian u32, u16, u16).
 * A thread per stream connects and then reads into the stream's buffer. */
enum { E_AGAIN_ = 11, E_MFILE_ = 24, E_PIPE_ = 32, E_CONNRESET_ = 104, E_ISCONN_ = 106, E_NOTCONN_ = 107,
       E_CONNREFUSED_ = 111, E_ALREADY_ = 114, E_INPROGRESS_ = 115 };
enum { POLLIN_ = 0x1, POLLOUT_ = 0x4, POLLERR_ = 0x8, POLLHUP_ = 0x10, POLLRDNORM_ = 0x40 };
enum { S_CONNECTING = 1, S_OPEN, S_FAILED };
#define CONNECT_MS 5000

typedef struct Stream {
    i32 fd;                  /* 0: free */
    u32 gen;                 /* (tells a reused slot from the one a thread was started for) */
    int state, err, eof, reported;
    SOCKET s;
    u8 *buf;
    u32 len, cap;
    u32 vip;
    u16 vport, dport;
} Stream;
#define STREAMS 16
static Stream streams[STREAMS];
static SRWLOCK stream_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE stream_change = CONDITION_VARIABLE_INIT;
static u32 stream_gen;

static Stream *stream_of(i32 fd) {
    for (int i = 0; i < STREAMS; i++)
        if (streams[i].fd == fd) return &streams[i];
    return NULL;
}

static void changed(void) {
    WakeAllConditionVariable(&stream_change);
    net_wake();
}

/* The lobby's real address: host[:port], from SSBU_NET_RELAY or online_server.txt (net_relay.c). */
static int lobby_address(struct sockaddr_in *out) {
    const char *v = net_relay_address();
    char host[256];
    if (!v || !*v) return 0;
    snprintf(host, sizeof host, "%s", v);
    u16 port = NET_DEFAULT_PORT;
    char *colon = strrchr(host, ':');
    if (colon) { port = (u16)atoi(colon + 1); *colon = 0; }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) return 0;
    *out = *(struct sockaddr_in *)res->ai_addr;
    out->sin_port = htons(port);
    freeaddrinfo(res);
    return 1;
}

/* Connects s to the lobby within CONNECT_MS (a lobby that does not answer fails the connect, not
 * hangs it), and leaves it blocking. */
#ifdef _WIN32
static int connect_within(SOCKET s, const struct sockaddr_in *at) {
    u_long nb = 1;
    int ok = ioctlsocket(s, FIONBIO, &nb) == 0;
    if (ok && connect(s, (const struct sockaddr *)at, sizeof *at) != 0) {
        fd_set w, e;
        FD_ZERO(&w); FD_ZERO(&e);
        FD_SET(s, &w); FD_SET(s, &e);
        struct timeval tv = {CONNECT_MS / 1000, CONNECT_MS % 1000 * 1000};
        ok = WSAGetLastError() == WSAEWOULDBLOCK && select(0, NULL, &w, &e, &tv) == 1 && FD_ISSET(s, &w);
    }
    nb = 0;
    return ok && ioctlsocket(s, FIONBIO, &nb) == 0;
}
#else
static int connect_within(SOCKET s, const struct sockaddr_in *at) {
    int flags = fcntl(s, F_GETFL, 0);
    int ok = flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
    if (ok && connect(s, (const struct sockaddr *)at, sizeof *at) != 0) {   /* (a failed one polls as writable, with SO_ERROR set) */
        struct pollfd p = {s, POLLOUT, 0};
        int err = 0;
        socklen_t n = sizeof err;
        ok = errno == EINPROGRESS && poll(&p, 1, CONNECT_MS) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &n) == 0 && !err;
    }
    return ok && fcntl(s, F_SETFL, flags) == 0;
}
#endif

static DWORD WINAPI stream_thread(void *arg) {
    Stream *st = arg;
    AcquireSRWLockExclusive(&stream_lock);
    i32 fd = st->fd;
    u32 gen = st->gen, vip = st->vip;
    u16 vport = st->vport, dport = st->dport;
    ReleaseSRWLockExclusive(&stream_lock);
    struct sockaddr_in at;
    SOCKET s = INVALID_SOCKET;
    int ok = lobby_address(&at);
    if (ok) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok = s != INVALID_SOCKET && connect_within(s, &at);
    }
    if (ok) {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        u8 head[16] = {'S', 'S', 'B', 'U', 'N', 'E', 'X', '1', (u8)(vip >> 24), (u8)(vip >> 16), (u8)(vip >> 8), (u8)vip,
                       (u8)(vport >> 8), (u8)vport, (u8)(dport >> 8), (u8)dport};
        ok = send(s, (const char *)head, sizeof head, 0) == (int)sizeof head;
    }
    AcquireSRWLockExclusive(&stream_lock);
    if (st->fd != fd || st->gen != gen) {   /* (closed meanwhile) */
        ReleaseSRWLockExclusive(&stream_lock);
        if (s != INVALID_SOCKET) closesocket(s);
        return 0;
    }
    if (ok) { st->s = s; st->state = S_OPEN; }
    else { st->state = S_FAILED; st->err = E_CONNREFUSED_; if (s != INVALID_SOCKET) closesocket(s); s = INVALID_SOCKET; }
    ReleaseSRWLockExclusive(&stream_lock);
    if (net_log()) fprintf(stderr, "[online] stream %d to port %u: %s\n", fd, dport, ok ? "connected to the lobby" : "the lobby did not answer");
    changed();
    if (!ok) return 0;
    char chunk[16384];
    for (;;) {
        int n = recv(s, chunk, sizeof chunk, 0);
        AcquireSRWLockExclusive(&stream_lock);
        if (st->fd != fd || st->gen != gen) { ReleaseSRWLockExclusive(&stream_lock); break; }
        if (n <= 0) {
            st->eof = 1;
            ReleaseSRWLockExclusive(&stream_lock);
            changed();
            break;
        }
        if (st->len + (u32)n > st->cap) {
            u32 cap = st->cap ? st->cap : 65536;
            while (cap < st->len + (u32)n) cap *= 2;
            u8 *b = realloc(st->buf, cap);
            if (!b) { st->eof = 1; ReleaseSRWLockExclusive(&stream_lock); changed(); break; }
            st->buf = b;
            st->cap = cap;
        }
        memcpy(st->buf + st->len, chunk, (size_t)n);
        st->len += (u32)n;
        ReleaseSRWLockExclusive(&stream_lock);
        changed();
    }
    return 0;
}

int online_stream_connect(i32 fd, u32 ip, u16 port, u32 my_ip, u16 my_port, int nonblock) {
    u32 server = online_ip();
    if (!server || ip != server) return E_CONNREFUSED_;   /* (no other stream reaches anything) */
    if (net_unreachable() || !my_ip) return E_CONNREFUSED_;   /* (the lobby is gone: Online is too) */
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st = stream_of(fd);
    if (st) {
        int e = st->state == S_OPEN ? E_ISCONN_ : E_ALREADY_;
        ReleaseSRWLockExclusive(&stream_lock);
        return e;
    }
    for (int i = 0; i < STREAMS && !st; i++)
        if (!streams[i].fd) st = &streams[i];
    if (!st) { ReleaseSRWLockExclusive(&stream_lock); return E_MFILE_; }
    memset(st, 0, sizeof *st);
    st->fd = fd;
    st->gen = ++stream_gen;
    st->state = S_CONNECTING;
    st->s = INVALID_SOCKET;
    st->vip = my_ip;
    st->vport = my_port;
    st->dport = port;
    ReleaseSRWLockExclusive(&stream_lock);
    HANDLE t = CreateThread(NULL, 0, stream_thread, st, 0, NULL);
    if (!t) { online_stream_close(fd); return E_CONNREFUSED_; }
    CloseHandle(t);
    if (nonblock) return E_INPROGRESS_;
    AcquireSRWLockExclusive(&stream_lock);
    while (st->fd == fd && st->state == S_CONNECTING) SleepConditionVariableSRW(&stream_change, &stream_lock, INFINITE, 0);
    int err = st->fd == fd && st->state == S_FAILED ? st->err : 0;
    if (err) st->reported = 1;
    ReleaseSRWLockExclusive(&stream_lock);
    return err;
}

i64 online_stream_send(i32 fd, const void *data, u64 len, int nonblock) {
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st = stream_of(fd);
    while (st && st->state == S_CONNECTING && !nonblock) {
        SleepConditionVariableSRW(&stream_change, &stream_lock, INFINITE, 0);
        st = stream_of(fd);
    }
    i64 r = !st ? -E_NOTCONN_ : st->state == S_CONNECTING ? -E_AGAIN_ : st->state != S_OPEN || st->eof ? -E_PIPE_ : 0;
    SOCKET s = st && !r ? st->s : INVALID_SOCKET;
    ReleaseSRWLockExclusive(&stream_lock);
    if (r) return r;
    const char *p = data;
    u64 left = len;
    while (left) {   /* (the host socket blocks: everything goes, in order) */
        int n = send(s, p, left > 0x10000 ? 0x10000 : (int)left, 0);
        if (n <= 0) return len - left ? (i64)(len - left) : -E_CONNRESET_;
        p += n;
        left -= (u64)n;
    }
    if (net_log() > 1) fprintf(stderr, "[online] stream %d: sent %llu bytes\n", fd, (unsigned long long)len);
    return (i64)len;
}

i64 online_stream_recv(i32 fd, void *buf, u64 len, int peek, int nonblock) {
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st;
    for (;;) {
        st = stream_of(fd);
        if (!st || st->len || st->eof || st->state == S_FAILED || nonblock) break;
        SleepConditionVariableSRW(&stream_change, &stream_lock, INFINITE, 0);
    }
    i64 r;
    if (!st) r = -E_NOTCONN_;
    else if (st->len) {
        u32 n = st->len < len ? st->len : (u32)len;
        memcpy(buf, st->buf, n);
        if (!peek) { memmove(st->buf, st->buf + n, st->len - n); st->len -= n; }
        r = n;
    } else if (st->eof) r = 0;
    else if (st->state == S_FAILED) r = -st->err;
    else r = -E_AGAIN_;
    ReleaseSRWLockExclusive(&stream_lock);
    if (net_log() > 1 && r > 0) fprintf(stderr, "[online] stream %d: received %lld bytes\n", fd, (long long)r);
    return r;
}

u16 online_stream_revents(i32 fd, u16 events) {
    AcquireSRWLockShared(&stream_lock);
    Stream *st = stream_of(fd);
    u16 r = 0;
    if (!st) r = POLLHUP_;
    else {
        if (st->state == S_OPEN && !st->eof) r |= events & POLLOUT_;
        if (st->len || st->eof) r |= events & (POLLIN_ | POLLRDNORM_);
        if (st->eof) r |= POLLHUP_;
        if (st->state == S_FAILED) r |= POLLERR_ | (events & POLLOUT_);
    }
    ReleaseSRWLockShared(&stream_lock);
    return r;
}

int online_stream_error(i32 fd) {
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st = stream_of(fd);
    int err = 0;
    if (st && st->state == S_FAILED && !st->reported) { err = st->err; st->reported = 1; }
    ReleaseSRWLockExclusive(&stream_lock);
    return err;
}

void online_stream_close(i32 fd) {
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st = stream_of(fd);
    SOCKET s = INVALID_SOCKET;
    if (st) {
        s = st->s;
        free(st->buf);
        memset(st, 0, sizeof *st);   /* (its thread sees its fd gone and stops) */
    }
    ReleaseSRWLockExclusive(&stream_lock);
    if (s != INVALID_SOCKET) { shutdown(s, SD_BOTH); closesocket(s); }
    if (st) changed();
}
