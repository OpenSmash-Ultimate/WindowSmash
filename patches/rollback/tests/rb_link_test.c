/* M6: hle_rb_link.c alone, on this PC's loopback: a game waiting to be found ("*") learns the other's
 * address from its first valid message and answers there; a game given an address follows the other
 * to another port (a router's doing), and messages from any other address are dropped unread.
 * M7: the host with two players found (3 players): each at its own address, rbl_send to both,
 * rbl_send_to one only, a player's slot not taken by another address.
 * Build: patches\rollback\tests\run_rb_net_test.bat (clang-cl, no game). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rbl_open(int local, int my_port, const char *host, int peer_port);
void rbl_close(void);
void rbl_players(int n);
void rbl_send(const void *buf, int len);
void rbl_send_to(int p, const void *buf, int len);
int rbl_recv(void *buf, int cap);
int rbl_accept(int p);
int rbl_has_peer(int p);
int rbl_who(void);
void rbl_assign(int p);
void rbl_stats(long long *s, long long *r, long long *e);
long long rbl_dropped(void);
double rbl_last_arrival_ms(void);
static double clock_ms(void) { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / (double)f.QuadPart; }

static int fails;
#define CHECK(c, what) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", what); fails++; } } while (0)

static SOCKET raw(const char *ip, int port) {   /* another game, as a plain socket */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port); inet_pton(AF_INET, ip, &a.sin_addr);
    if (bind(s, (struct sockaddr *)&a, sizeof a)) { closesocket(s); return INVALID_SOCKET; }
    DWORD ms = 300; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
    return s;
}
static void to(SOCKET s, int port, const char *msg) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    sendto(s, msg, (int)strlen(msg), 0, (struct sockaddr *)&a, sizeof a);
}
static int got(SOCKET s, const char *msg) {   /* the plain socket received msg */
    char b[64]; int n = recv(s, b, sizeof b - 1, 0);
    if (n <= 0) return 0;
    b[n] = 0; return !strcmp(b, msg);
}
static int link_read(char *b, int cap) {   /* rbl_recv, giving the message a moment to arrive */
    for (int i = 0; i < 50; i++) { int n = rbl_recv(b, cap - 1); if (n > 0) { b[n] = 0; return n; } Sleep(10); }
    return 0;
}

int main(void) {
    char b[64];
    long long s, r, e;
    _putenv("SSBU_RB_UPNP=0");   /* (a waiting link asks the router for its port: not this PC's router, in a test) */
    {   /* the host waiting to be found: nothing sent until found; then answers at the finder's own port */
        CHECK(rbl_open(0, 17801, "*", 17802), "open waiting");
        CHECK(!rbl_has_peer(1), "a peer before any message");
        rbl_send("early", 5);
        rbl_stats(&s, &r, &e);
        CHECK(s == 0, "sent before being found");
        SOCKET other = raw("127.0.0.1", 17809);   /* (a router's port, not 17802) */
        double sent_at = clock_ms();
        to(other, 17801, "hello");
        Sleep(100);   /* (read a frame or more after it came: its time is when it came) */
        CHECK(link_read(b, sizeof b) && !strcmp(b, "hello"), "the finder's message not read");
        double late = clock_ms() - rbl_last_arrival_ms(), trip = rbl_last_arrival_ms() - sent_at;
        CHECK(late >= 90 && trip >= 0 && trip < 20, "the message's time is when it was read, not when it came");
        printf("(arrival: %.2f ms after sending, read %.1f ms after arriving)\n", trip, late);
        CHECK(rbl_accept(1) == 1 && rbl_has_peer(1), "the finder not taken as player 2");
        CHECK(rbl_accept(1) == 0, "the same peer taken twice");
        rbl_send("answer", 6);
        CHECK(got(other, "answer"), "the answer did not reach the finder's port");
        SOCKET stranger = raw("127.0.0.2", 17810);   /* another address */
        if (stranger != INVALID_SOCKET) {   /* (the host reads it - it may be a joiner - but it is no player's: Host/Join) */
            to(stranger, 17801, "noise");
            CHECK(link_read(b, sizeof b) && rbl_who() == -1, "a stranger's message taken as a player's");
            CHECK(rbl_accept(1) == -1, "a stranger taken as player 2");
            closesocket(stranger);
        } else printf("(127.0.0.2 not usable here: the stranger check left out)\n");
        closesocket(other);
        rbl_close();
    }
    {   /* a game that is not the host drops a message from anyone but the host */
        CHECK(rbl_open(1, 17801, "127.0.0.1:17805", 7701), "open as a joiner");
        SOCKET stranger = raw("127.0.0.2", 17811);
        if (stranger != INVALID_SOCKET) {
            to(stranger, 17801, "noise");
            Sleep(50);
            CHECK(rbl_recv(b, sizeof b) == 0, "a stranger's message read by a joiner");
            CHECK(rbl_dropped() == 1, "a stranger's message not counted as dropped");
            closesocket(stranger);
        }
        rbl_close();
    }
    {   /* a game given the host's name and port: followed to the port the host's messages really come from */
        CHECK(rbl_open(1, 17801, "localhost:17805", 7701), "open with localhost:17805");
        SOCKET told = raw("127.0.0.1", 17805), moved = raw("127.0.0.1", 17806);
        rbl_send("one", 3);
        CHECK(got(told, "one"), "the given port did not get the first message");
        to(moved, 17801, "from 17806");
        CHECK(link_read(b, sizeof b) && !strcmp(b, "from 17806"), "a message from the host's address, another port, not read");
        rbl_send("two", 3);
        CHECK(got(told, "two"), "the peer moved before the message was found valid");
        CHECK(rbl_accept(0) == 1, "the new port not taken");
        rbl_send("three", 5);
        CHECK(got(moved, "three"), "the answer did not follow to the new port");
        closesocket(told); closesocket(moved);
        rbl_close();
    }
    {   /* M7: the host, 3 players: two found, each answered at its own address; one alone; no slot taken twice */
        CHECK(rbl_open(0, 17801, "*", 7702), "open waiting (3 players)");
        rbl_players(3);
        SOCKET p2 = raw("127.0.0.1", 17821), p3 = raw("127.0.0.1", 17831);
        to(p2, 17801, "i am 2");
        CHECK(link_read(b, sizeof b) && !strcmp(b, "i am 2"), "player 2's first message not read");
        CHECK(rbl_accept(1) == 1, "player 2 not taken");
        to(p3, 17801, "i am 3");   /* (the same IP as player 2, as two friends in one house: told apart by port) */
        CHECK(link_read(b, sizeof b) && !strcmp(b, "i am 3"), "player 3's first message not read (a slot still free)");
        CHECK(rbl_accept(2) == 1 && rbl_has_peer(2), "player 3 not taken");
        to(p3, 17801, "i am 2 now");   /* (a message from player 3's address saying it is player 2: not taken) */
        CHECK(link_read(b, sizeof b), "player 3's second message not read");
        CHECK(rbl_accept(1) == -1, "player 3's address taken as player 2's");
        rbl_send("to all", 6);
        CHECK(got(p2, "to all") && got(p3, "to all"), "a message to all did not reach both");
        rbl_send_to(2, "to 3", 4);
        CHECK(got(p3, "to 3"), "a message to player 3 did not reach it");
        CHECK(!got(p2, "to 3"), "a message to player 3 reached player 2");
        /* (Host/Join: every slot known, a new address is still read - a friend's game started again - but is no
         * player's until the host gives it a number: rbl_assign) */
        to(p2, 17801, "who am i");
        CHECK(link_read(b, sizeof b) && rbl_who() == 1, "player 2's address not told as player 2's");
        SOCKET again = raw("127.0.0.1", 17822);   /* (player 2's game again, on another port) */
        to(again, 17801, "back again");
        CHECK(link_read(b, sizeof b) && !strcmp(b, "back again"), "a new address not read with every player found");
        CHECK(rbl_who() == -1, "a new address taken for a player before it was given a number");
        rbl_assign(1);
        CHECK(rbl_who() == 1, "the new address not player 2's after rbl_assign");
        rbl_send_to(1, "welcome back", 12);
        CHECK(got(again, "welcome back"), "player 2's messages did not follow to its new address");
        closesocket(again);
        closesocket(p2); closesocket(p3);
        rbl_close();
    }
    CHECK(!rbl_open(1, 17801, "no-such-host.invalid", 7701), "a name that is no address opened");
    rbl_close();
    CHECK(!rbl_open(1, 17801, "*", 7701), "a player other than the host waiting to be found");
    rbl_close();
    printf("link test: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
