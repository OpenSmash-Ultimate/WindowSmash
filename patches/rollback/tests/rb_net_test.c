/* M5: rb_net.c alone. Two peers run a toy fight (a state hashed from both pads each frame) over a
 * link that drops, delays and reorders messages; each rolls back on its wrong guesses. Passes when
 * both peers' confirmed states agree frame by frame, equal a run with every pad known, and no
 * rollback reached back further than RBN_MAX_BACK.
 * M7: up to 4 peers (argv[3]), as the game will play 3+: each sends to the host (peer A, player 1) only,
 * and the host passes every other peer's messages on to the rest (as received: the same bytes, again
 * lost, delayed and reordered on the way). Usage: rb_net_test <delay> [-|slow|norollback] [players].
 * Build: patches\rollback\tests\run_rb_net_test.bat (clang-cl, no game). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/hle_rb_net.h"

#define FRAMES 4000
#define TICKS 6000

static uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdull; }
static uint64_t pad_hash(const RbnPad *p) { return mix(mix(p->buttons, (uint16_t)p->lx | (uint64_t)(uint16_t)p->ly << 16), (uint16_t)p->lt); }
static uint32_t rnd_state = 12345;
static uint32_t rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }

/* what a player holds when it reads its pad at frame f: changes every few frames, sticks often in the rest zone */
static RbnPad read_pad(int who, uint32_t f) {
    uint64_t h = mix(who + 1, f / (uint32_t)(4 + who * 3));
    RbnPad p = {0};
    p.buttons = h & 0x1f;
    p.lx = (int16_t)((h >> 8) % 3 == 0 ? (int)((h >> 16) % 1200) - 600 : (int)((h >> 16) % 65535) - 32767);   /* (noise within the rest zone: no rollback) */
    p.ly = (int16_t)((int)((h >> 32) % 65535) - 32767);
    p.lt = (int16_t)((h >> 48) & 1 ? 30000 : 100);
    return p;
}

typedef struct Msg { int to, at; uint8_t bytes[RBN_MSG_MAX]; size_t len; } Msg;
static Msg *link_q; static int nlink, link_cap;

#define TP 4   /* (players at most in the test) */
typedef struct Peer {
    int who, start;            /* player index; the tick it starts at */
    void *net;                 /* its rb_net session */
    uint32_t cur;              /* the last frame run */
    uint64_t state[RBN_RING];  /* the state after each frame (by frame & mask) */
    uint64_t final[FRAMES + 64];   /* the state after each confirmed frame */
    uint32_t final_n;
    uint32_t acked_by[TP];     /* the newest frame of ours each player has all of */
    uint32_t heard[TP];        /* the newest frame each player said it was running */
    uint64_t peer_check[TP][FRAMES + 64]; uint8_t has_check[TP][FRAMES + 64];
    RbnPad used_local[FRAMES + 64];   /* the pad our player has at each frame (after the delay) */
    long rollbacks, rerun, waits, stalls, max_back, desyncs, checks, relayed;
} Peer;
static Peer peers[TP];
static int np = 2;     /* (players) */
static int sabotage;   /* (argv[2] "norollback": wrong guesses kept, the test must FAIL) */
static int slow;       /* (argv[2] "slow", M6: B misses one tick in 6 for the first half, as a PC at ~20 ms a frame) */
static long long msg_bytes, msg_pads, msgs;   /* (M6: PADS sizes, runs against 20 bytes a pad) */
static size_t net_size;

static void use(Peer *p) { size_t n; memcpy(rbn_state(&n), p->net, net_size); }
static void keep(Peer *p) { size_t n; memcpy(p->net, rbn_state(&n), net_size); }

static uint64_t step(Peer *p, uint32_t f) {
    uint64_t s = f > 1 ? p->state[(f - 1) & (RBN_RING - 1)] : 0x1234;
    for (int k = 0; k < np; k++) { RbnPad a = rbn_get(k, f, NULL); s = mix(s, pad_hash(&a)); }
    return s;
}
/* a message on its way: lost 1 in 10, else there 1..6 ticks on (so out of order too) */
static void post(int to, int t, const uint8_t *bytes, size_t len) {
    if (!(rnd() % 10)) return;
    if (nlink == link_cap) link_q = realloc(link_q, sizeof *link_q * (link_cap = link_cap ? link_cap * 2 : 256));
    Msg *q = &link_q[nlink++];
    memcpy(q->bytes, bytes, len); q->len = len;
    q->to = to; q->at = t + 1 + (int)(rnd() % 6);
}

static void tick(Peer *p, int t) {
    use(p);
    /* the messages that arrived */
    for (int i = 0; i < nlink;) {
        if (link_q[i].to != p->who || link_q[i].at > t) { i++; continue; }
        static RbnMsg m;
        Msg got = link_q[i];
        link_q[i] = link_q[--nlink];
        if (!rbn_parse(got.bytes, got.len, &m)) { fprintf(stderr, "FAIL: a message did not parse\n"); exit(1); }
        int j = m.player;
        if (j == p->who || j >= np) { fprintf(stderr, "FAIL: a message from player %d at player %d\n", j, p->who); exit(1); }
        if (p->who == 0) for (int k = 1; k < np; k++) if (k != j) { post(k, t, got.bytes, got.len); p->relayed++; }   /* (the host passes it on) */
        for (int k = 0; k < m.count; k++) rbn_remote(j, m.first + (uint32_t)k, m.pads[k]);
        if (m.ack[p->who] > p->acked_by[j]) p->acked_by[j] = m.ack[p->who];
        if (m.frame > p->heard[j]) p->heard[j] = m.frame;
        if (m.check_frame && m.check_frame < FRAMES + 64) { p->peer_check[j][m.check_frame] = m.check; p->has_check[j][m.check_frame] = 1; }
        /* (each side's advantage as each measures it, late alike: the difference cancels the trip) */
        rbn_sync_sample(j, (int32_t)p->cur - (int32_t)(p->heard[j] + 2), (int32_t)m.frame - (int32_t)(m.heard[p->who] + 2));
    }
    /* a wrong guess: back to before it, run again */
    uint32_t m = rbn_take_mismatch();
    if (m && m <= p->cur && !sabotage) {
        long back = (long)(p->cur - m + 1);
        if (back > p->max_back) p->max_back = back;
        p->rollbacks++;
        for (uint32_t f = m; f <= p->cur; f++) { p->state[f & (RBN_RING - 1)] = step(p, f); p->rerun++; }
    }
    /* the next frame, unless waiting for the peer to catch up or too far past its pads */
    uint32_t next = p->cur + 1;
    if (next <= FRAMES) {
        if (rbn_sync_wait(next)) p->waits++;
        else if (!rbn_can_run(next)) p->stalls++;
        else {
            p->used_local[next] = rbn_local(next, read_pad(p->who, next));
            p->state[next & (RBN_RING - 1)] = step(p, next);
            p->cur = next;
        }
    }
    /* frames now final: kept, and checked against the peer's hash */
    uint32_t c = rbn_confirmed();
    if (c > p->cur) c = p->cur;
    while (p->final_n < c) { p->final_n++; p->final[p->final_n] = p->state[p->final_n & (RBN_RING - 1)]; }
    for (int j = 0; j < np; j++)
        for (uint32_t f = 1; f <= p->final_n; f++)
            if (p->has_check[j][f]) { p->checks++; if ((uint32_t)p->final[f] != p->peer_check[j][f]) p->desyncs++; p->has_check[j][f] = 0; }
    /* our pads some player hasn't got (from the oldest any lacks, every time: messages get lost), our frame, what we
     * have of each, and our hash */
    static RbnMsg out;
    memset(&out, 0, sizeof out);
    out.type = RBN_MSG_PADS; out.player = p->who;
    uint32_t low = ~0u;
    for (int j = 0; j < np; j++) if (j != p->who && p->acked_by[j] < low) low = p->acked_by[j];
    out.first = low + 1;
    for (uint32_t f = out.first; f <= rbn_local_newest() && out.count < RBN_MAX_PADS; f++) rbn_local_get(f, &out.pads[out.count++]);
    for (int j = 0; j < np; j++) if (j != p->who) { out.ack[j] = rbn_remote_newest(j); out.heard[j] = p->heard[j]; }
    out.frame = p->cur;
    if (p->final_n) { out.check_frame = p->final_n; out.check = (uint32_t)p->final[p->final_n]; }
    static uint8_t bytes[RBN_MSG_MAX];
    size_t len = rbn_build(bytes, sizeof bytes, &out);
    if (!len) { fprintf(stderr, "FAIL: a message did not build\n"); exit(1); }
    msg_bytes += (long long)len; msg_pads += out.count; msgs++;
    if (p->who == 0) { for (int k = 1; k < np; k++) post(k, t, bytes, len); }   /* (the host: to everyone) */
    else post(0, t, bytes, len);                                               /* (the others: to the host) */
    keep(p);
}

int main(int argc, char **argv) {
    int d = argc > 1 ? atoi(argv[1]) : 2;
    {   /* HELLO: built and parsed back the same; a wrong length is refused */
        RbnHello h = {1, RBN_VERSION, 1, 3, 0x0123456789abcdefull, 0x1122334455667788ull, 0x99aabbccddeeff00ull, 123456}, g;
        uint8_t b[RBN_HELLO_BYTES];
        size_t n = rbn_hello_build(b, sizeof b, &h);
        if (n != RBN_HELLO_BYTES || !rbn_hello_parse(b, n, &g) || g.player != 1 || g.version != RBN_VERSION || !g.agreed || g.seed != h.seed ||
            g.input_delay != 3 || g.t != h.t || g.echo != h.echo || g.hold != h.hold ||
            rbn_hello_parse(b, n - 1, &g)) { fprintf(stderr, "FAIL: HELLO round trip\n"); return 1; }
        /* (Host/Join: a joiner with no number yet, its join id in the seed; the host's answer saying its number) */
        RbnHello j = {-1, RBN_VERSION, 0, 0, 0xfeedfacecafebeefull}, k;
        n = rbn_hello_build(b, sizeof b, &j);
        if (n != RBN_HELLO_BYTES || b[1] != 0xFF || !rbn_hello_parse(b, n, &k) || k.player != -1 || k.seed != j.seed || k.assign) {
            fprintf(stderr, "FAIL: a joiner's HELLO round trip\n"); return 1;
        }
        RbnHello a = {0, RBN_VERSION, 0, 0, 0}; a.players = 4; a.assign = 3;
        n = rbn_hello_build(b, sizeof b, &a);
        if (!n || !rbn_hello_parse(b, n, &k) || k.player != 0 || k.players != 4 || k.assign != 3) { fprintf(stderr, "FAIL: the host's assigning HELLO round trip\n"); return 1; }
        RbnHello bad = {-2, RBN_VERSION};
        if (rbn_hello_build(b, sizeof b, &bad)) { fprintf(stderr, "FAIL: a HELLO from player -2 built\n"); return 1; }
        /* (a version 1 hello, 16 bytes: read far enough to tell its version) */
        uint8_t old[16] = {RBN_MSG_HELLO, 0, 1, 0};
        if (!rbn_hello_parse(old, sizeof old, &g) || g.version != 1) { fprintf(stderr, "FAIL: a version 1 HELLO not told apart\n"); return 1; }
    }
    {   /* the delay from the round trip: 1 on a home network, one more each 50 ms, at most 5 */
        static const struct { double rtt; int d; } k[] = {{0, 1}, {3, 1}, {49.9, 1}, {50, 2}, {99, 2}, {120, 3}, {180, 4}, {240, 5}, {900, 5}, {-1, 1}};
        for (unsigned i = 0; i < sizeof k / sizeof *k; i++)
            if (rbn_delay_for_rtt(k[i].rtt) != k[i].d) { fprintf(stderr, "FAIL: delay for %.1f ms is %d, not %d\n", k[i].rtt, rbn_delay_for_rtt(k[i].rtt), k[i].d); return 1; }
    }
    {   /* CHOICE: built and parsed back the same */
        RbnChoice c = {0, 3, 1, 1, 0xc100000000123456ull, {0}}, d;
        for (int k = 0; k < 0x20; k++) c.stage[k] = (uint8_t)(k * 7);
        uint8_t b[RBN_CHOICE_BYTES];
        size_t n = rbn_choice_build(b, sizeof b, &c);
        if (n != RBN_CHOICE_BYTES || !rbn_choice_parse(b, n, &d) || d.player != 0 || d.colour != 3 || !d.has_stage || d.hazards != 1 ||
            d.chara != c.chara || memcmp(d.stage, c.stage, 0x20) || rbn_choice_parse(b, n - 1, &d)) { fprintf(stderr, "FAIL: CHOICE round trip%c", 10); return 1; }
    }
    {   /* RULES (streamlining 2): a chunk built and parsed back the same; an ask with no data; a chunk past the blob,
         * or a wrong length, refused */
        static RbnRules r, g;
        static uint8_t b[RBN_RULES_HEAD + RBN_RULES_CHUNK];
        r.player = 0; r.id = 0xa1b2c3d4u; r.total = 0x5ee; r.off = RBN_RULES_CHUNK; r.len = r.total - r.off;   /* (the last chunk) */
        for (int k = 0; k < r.len; k++) r.data[k] = (uint8_t)(k * 13 + 1);
        size_t n = rbn_rules_build(b, sizeof b, &r);
        if (n != RBN_RULES_HEAD + (size_t)r.len || !rbn_rules_parse(b, n, &g) || g.player != 0 || g.ask || g.id != r.id || g.total != r.total ||
            g.off != r.off || g.len != r.len || memcmp(g.data, r.data, (size_t)r.len) || rbn_rules_parse(b, n - 1, &g)) { fprintf(stderr, "FAIL: RULES round trip%c", 10); return 1; }
        RbnRules ask = {2, 1};
        n = rbn_rules_build(b, sizeof b, &ask);
        if (n != RBN_RULES_HEAD || !rbn_rules_parse(b, n, &g) || !g.ask || g.player != 2 || g.len) { fprintf(stderr, "FAIL: a RULES ask%c", 10); return 1; }
        r.off = 0x500;   /* (0x500 + 0x300 > 0x5ee) */
        if (rbn_rules_build(b, sizeof b, &r)) { fprintf(stderr, "FAIL: a RULES chunk past the blob built%c", 10); return 1; }
    }
    {   /* PADS in runs (M6): 64 pads held alike are one run; a mixed sequence comes back pad for pad; runs that don't
         * add up to the count are refused */
        static RbnMsg m, g;
        static uint8_t b[RBN_MSG_MAX];
        m.type = RBN_MSG_PADS; m.player = 1; m.count = RBN_MAX_PADS; m.first = 100; m.frame = 103;
        for (int k = 0; k < RBN_PLAYERS; k++) { m.ack[k] = 90u + (unsigned)k; m.heard[k] = 200u + (unsigned)k; }
        for (int k = 0; k < m.count; k++) { m.pads[k].buttons = 0x5; m.pads[k].lx = -32767; }
        size_t n = rbn_build(b, sizeof b, &m);
        if (n != RBN_HEAD + RBN_RUN_BYTES || !rbn_parse(b, n, &g) || g.count != m.count || memcmp(g.pads, m.pads, sizeof m.pads)) {
            fprintf(stderr, "FAIL: 64 pads alike are not one run (%zu bytes)%c", n, 10); return 1;
        }
        for (int k = 0; k < m.count; k++) { m.pads[k].buttons = (uint64_t)(k / 5 % 3); m.pads[k].lx = (int16_t)(k % 7 == 0 ? 9000 : 0); }
        n = rbn_build(b, sizeof b, &m);
        if (!n || !rbn_parse(b, n, &g) || g.count != m.count || memcmp(g.pads, m.pads, sizeof m.pads) || g.first != 100 || g.frame != 103 ||
            memcmp(g.ack, m.ack, sizeof m.ack) || memcmp(g.heard, m.heard, sizeof m.heard)) {
            fprintf(stderr, "FAIL: PADS in runs round trip%c", 10); return 1;
        }
        printf("PADS: 64 mixed pads in %d runs, %zu bytes (were %d)\n", b[3], n, RBN_HEAD + RBN_MAX_PADS * RBN_PAD_BYTES);
        b[RBN_HEAD] = (uint8_t)(b[RBN_HEAD] + 1);   /* (the first run one frame longer: more than the count) */
        if (rbn_parse(b, n, &g)) { fprintf(stderr, "FAIL: runs past the count accepted%c", 10); return 1; }
        b[RBN_HEAD] = 0;
        if (rbn_parse(b, n, &g)) { fprintf(stderr, "FAIL: a run of 0 frames accepted%c", 10); return 1; }
    }
    sabotage = argc > 2 && !strcmp(argv[2], "norollback");
    slow = argc > 2 && !strcmp(argv[2], "slow");
    np = argc > 3 ? atoi(argv[3]) : 2;
    if (np < 2 || np > TP) { fprintf(stderr, "FAIL: 2 to %d players\n", TP); return 1; }
    rbn_state(&net_size);
    for (int i = 0; i < np; i++) {
        peers[i].who = i; peers[i].start = 7 * i;   /* (each starts 7 ticks after the one before: the frame advantage evens it out) */
        peers[i].net = calloc(1, net_size);
        rbn_reset(np, i, d); rbn_sync_reset(); keep(&peers[i]);
    }
    for (int t = 0; t < TICKS; t++)
        for (int i = 0; i < np; i++) if (t >= peers[i].start && !(slow && i == 1 && t < TICKS / 2 && t % 6 == 0)) tick(&peers[i], t);
    /* a run with every pad known */
    static uint64_t ideal[FRAMES + 1];
    uint64_t s = 0x1234;
    for (uint32_t f = 1; f <= FRAMES; f++) {
        for (int k = 0; k < np; k++) s = mix(s, pad_hash(&peers[k].used_local[f]));
        ideal[f] = s;
    }
    int ok = 1;
    uint32_t n = ~0u;
    for (int i = 0; i < np; i++) if (peers[i].final_n < n) n = peers[i].final_n;
    for (uint32_t f = 1; f <= n && ok; f++)
        for (int i = 0; i < np; i++)
            if (peers[i].final[f] != ideal[f]) { fprintf(stderr, "FAIL: frame %u differs at peer %c\n", f, 'A' + i); ok = 0; break; }
    for (int i = 0; i < np; i++) {
        Peer *p = &peers[i];
        printf("peer %c: ran to %u, %u confirmed; %ld rollbacks (%ld frames again, deepest %ld), %ld waits, %ld stalls; %ld hashes checked, %ld differ%s\n",
               'A' + i, p->cur, p->final_n, p->rollbacks, p->rerun, p->max_back, p->waits, p->stalls, p->checks, p->desyncs, i == 0 && np > 2 ? " (the host)" : "");
        if (p->max_back > RBN_MAX_BACK || p->desyncs || p->final_n < FRAMES - 64) ok = 0;
    }
    if (msgs) printf("messages: %lld, %.1f pads and %.1f bytes each on average (%.1f without runs)\n", msgs, (double)msg_pads / msgs,
                     (double)msg_bytes / msgs, RBN_HEAD + (double)msg_pads / msgs * RBN_PAD_BYTES);
    printf("delay %d%s, %d players: %u frames compared with the run with all pads known: %s\n", d, slow ? " (B slow)" : "", np, n, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
