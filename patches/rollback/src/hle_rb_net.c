/* Rollback build, M5: the netcode's own part (see rb_net.h). Each table is a ring of RBN_RING
 * entries by frame & (RBN_RING - 1), and every entry keeps its frame number too, so one left from
 * an older lap is never taken for the current frame. */
#include <string.h>
#include "hle_rb_net.h"

#define MASK (RBN_RING - 1)

typedef struct Remote {
    RbnPad pad[RBN_RING]; uint32_t at[RBN_RING];       /* real pads received */
    RbnPad guess[RBN_RING]; uint32_t guess_at[RBN_RING];   /* the guess a frame ran on, until its real pad comes */
    uint32_t newest;                                   /* every frame up to this one received */
    uint32_t gone;                                     /* (v0.9.2) gone from the fight after this frame: none from then on, 0: here */
} Remote;
#define SAMPLES 32
/* everything a session keeps, in one place (rbn_state: the test runs two sessions in one process) */
static struct Net {
    int nplayers, local_player, delay;
    uint32_t mismatch;   /* the earliest frame run on a wrong guess, not yet taken (0: none) */
    RbnPad local_pad[RBN_RING];
    uint32_t local_at[RBN_RING], local_newest;
    Remote remote[RBN_PLAYERS];
    int32_t diff[RBN_PLAYERS][SAMPLES];   /* (frame advantage over each player: local - remote, the newest SAMPLES) */
    int ndiff[RBN_PLAYERS], next_diff[RBN_PLAYERS], owed;
} N;
void *rbn_state(size_t *size) { *size = sizeof N; return &N; }
#define nplayers N.nplayers
#define local_player N.local_player
#define delay N.delay
#define mismatch N.mismatch
#define local_pad N.local_pad
#define local_at N.local_at
#define local_newest N.local_newest
#define remote N.remote
#define diff N.diff
#define ndiff N.ndiff
#define next_diff N.next_diff
#define owed N.owed

void rbn_reset(int n, int local, int d) {
    memset(local_pad, 0, sizeof local_pad); memset(local_at, 0, sizeof local_at); memset(remote, 0, sizeof remote);
    nplayers = n < 1 ? 1 : n > RBN_PLAYERS ? RBN_PLAYERS : n;
    local_player = local;
    delay = d < 0 ? 0 : d > 15 ? 15 : d;
    local_newest = mismatch = 0;
}
int rbn_delay(void) { return delay; }
int rbn_players(void) { return nplayers; }
int rbn_local_player(void) { return local_player; }

static int16_t rest(int16_t v) { return v >= -RBN_REST && v <= RBN_REST ? 0 : v; }
RbnPad rbn_rest(RbnPad p) {
    p.lx = rest(p.lx); p.ly = rest(p.ly); p.rx = rest(p.rx); p.ry = rest(p.ry);
    p.lt = p.lt > 0x27FF ? 32767 : 0; p.rt = p.rt > 0x27FF ? 32767 : 0;   /* (the game only asks past 0x27FF: hle_hid.c) */
    return p;
}
int rbn_same(const RbnPad *a, const RbnPad *b) {
    return a->buttons == b->buttons && a->lx == b->lx && a->ly == b->ly && a->rx == b->rx && a->ry == b->ry && a->lt == b->lt && a->rt == b->rt;
}

RbnPad rbn_local(uint32_t frame, RbnPad now) {
    static const RbnPad none;
    uint32_t at = frame + (uint32_t)delay;
    for (uint32_t f = local_newest + 1; f < at; f++) { local_pad[f & MASK] = none; local_at[f & MASK] = f; }   /* the first frames: nothing held */
    if (at > local_newest) { local_pad[at & MASK] = rbn_rest(now); local_at[at & MASK] = at; local_newest = at; }
    return local_at[frame & MASK] == frame ? local_pad[frame & MASK] : none;
}
int rbn_local_get(uint32_t frame, RbnPad *out) {
    if (!frame || frame > local_newest || local_at[frame & MASK] != frame) return 0;
    *out = local_pad[frame & MASK];
    return 1;
}
uint32_t rbn_local_newest(void) { return local_newest; }

void rbn_remote(int p, uint32_t frame, RbnPad pad) {
    if (p < 0 || p >= nplayers || p == local_player || !frame) return;
    Remote *r = &remote[p];
    if (frame + RBN_RING <= r->newest + 1 || frame > r->newest + RBN_RING) return;   /* outside the ring */
    if (r->at[frame & MASK] == frame) return;   /* (sent again: it already came) */
    if (r->gone && frame > r->gone) return;    /* (past its cutoff: none, on every game alike) */
    pad = rbn_rest(pad);
    r->pad[frame & MASK] = pad; r->at[frame & MASK] = frame;
    while (r->at[(r->newest + 1) & MASK] == r->newest + 1) r->newest++;
    if (r->guess_at[frame & MASK] == frame) {
        if (!rbn_same(&r->guess[frame & MASK], &pad) && (!mismatch || frame < mismatch)) mismatch = frame;
        r->guess_at[frame & MASK] = 0;
    }
}
uint32_t rbn_remote_newest(int p) { return p >= 0 && p < nplayers ? remote[p].newest : 0; }

RbnPad rbn_get(int p, uint32_t frame, int *guessed) {
    static const RbnPad none;
    if (guessed) *guessed = 0;
    if (p < 0 || p >= nplayers || !frame) return none;
    if (p == local_player) { RbnPad out = none; rbn_local_get(frame, &out); return out; }
    Remote *r = &remote[p];
    if (r->gone && frame > r->gone) return none;
    if (r->at[frame & MASK] == frame) return r->pad[frame & MASK];
    /* not here yet: the newest real pad before it, held (a re-run of the frame guesses again, maybe better) */
    RbnPad g = none;
    for (uint32_t f = frame - 1; f && f + RBN_RING > frame; f--) if (r->at[f & MASK] == f) { g = r->pad[f & MASK]; break; }
    r->guess[frame & MASK] = g; r->guess_at[frame & MASK] = frame;
    if (guessed) *guessed = 1;
    return g;
}

uint32_t rbn_take_mismatch(void) { uint32_t m = mismatch; mismatch = 0; return m; }
void rbn_put_mismatch(uint32_t m) { if (m && (!mismatch || m < mismatch)) mismatch = m; }

/* (a gone player's pads are all known once its real ones up to the cutoff came: none after it) */
static uint32_t known_up_to(int p) { return remote[p].gone && remote[p].newest >= remote[p].gone ? 0xFFFFFFF0u : remote[p].newest; }
uint32_t rbn_confirmed(void) {
    uint32_t c = local_newest;
    for (int p = 0; p < nplayers; p++) if (p != local_player && known_up_to(p) < c) c = known_up_to(p);
    return c;
}
int rbn_can_run(uint32_t frame) {
    for (int p = 0; p < nplayers; p++) if (p != local_player && frame > known_up_to(p) + RBN_MAX_BACK) return 0;
    return 1;
}
void rbn_gone(int p, uint32_t after, uint32_t ran) {
    if (p < 0 || p >= nplayers || p == local_player || !after) return;
    Remote *r = &remote[p];
    if (r->gone && r->gone <= after) return;   /* (the earliest cutoff heard holds) */
    r->gone = after;
    /* frames past the cutoff this game already ran (up to `ran`) on a guess, or on a real pad that came before it
     * left, other than none: run again */
    static const RbnPad none;
    for (uint32_t f = after + 1; f <= ran && f < after + RBN_RING; f++) {
        int other = 0;
        if (r->guess_at[f & MASK] == f) { other = !rbn_same(&r->guess[f & MASK], &none); r->guess_at[f & MASK] = 0; }
        else if (r->at[f & MASK] == f) other = !rbn_same(&r->pad[f & MASK], &none);
        if (other && (!mismatch || f < mismatch)) mismatch = f;
    }
}
uint32_t rbn_gone_after(int p) { return p >= 0 && p < nplayers ? remote[p].gone : 0; }

/* ---- the wire ---- */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

size_t rbn_build(uint8_t *b, size_t cap, const RbnMsg *m) {
    if (m->type != RBN_MSG_PADS || m->count < 0 || m->count > RBN_MAX_PADS || m->player < 0 || m->player >= RBN_PLAYERS) return 0;
    if (cap < RBN_HEAD) return 0;
    put32(b + 4, m->first); put32(b + 8, m->frame); put32(b + 12, m->check_frame); put32(b + 16, m->check);
    for (int p = 0; p < RBN_PLAYERS; p++) { put32(b + 20 + 4 * p, m->ack[p]); put32(b + 20 + 4 * RBN_PLAYERS + 4 * p, m->heard[p]); }
    size_t n = RBN_HEAD;
    int runs = 0;
    for (int i = 0; i < m->count;) {
        int k = 1;
        while (i + k < m->count && k < 255 && rbn_same(&m->pads[i], &m->pads[i + k])) k++;
        if (n + RBN_RUN_BYTES > cap) return 0;
        uint8_t *q = b + n;
        const RbnPad *p = &m->pads[i];
        q[0] = (uint8_t)k;
        put64(q + 1, p->buttons);
        put16(q + 9, (uint16_t)p->lx); put16(q + 11, (uint16_t)p->ly); put16(q + 13, (uint16_t)p->rx);
        put16(q + 15, (uint16_t)p->ry); put16(q + 17, (uint16_t)p->lt); put16(q + 19, (uint16_t)p->rt);
        n += RBN_RUN_BYTES; runs++; i += k;
    }
    b[0] = RBN_MSG_PADS; b[1] = (uint8_t)m->player; b[2] = (uint8_t)m->count; b[3] = (uint8_t)runs;
    return n;
}
int rbn_parse(const uint8_t *b, size_t len, RbnMsg *m) {
    if (len < RBN_HEAD || b[0] != RBN_MSG_PADS || b[1] >= RBN_PLAYERS || b[2] > RBN_MAX_PADS) return 0;
    if (len != RBN_HEAD + (size_t)b[3] * RBN_RUN_BYTES) return 0;
    m->type = RBN_MSG_PADS; m->player = b[1]; m->count = b[2];
    m->first = get32(b + 4); m->frame = get32(b + 8); m->check_frame = get32(b + 12); m->check = get32(b + 16);
    for (int p = 0; p < RBN_PLAYERS; p++) { m->ack[p] = get32(b + 20 + 4 * p); m->heard[p] = get32(b + 20 + 4 * RBN_PLAYERS + 4 * p); }
    int i = 0;
    for (int r = 0; r < b[3]; r++) {
        const uint8_t *q = b + RBN_HEAD + (size_t)r * RBN_RUN_BYTES;
        int k = q[0];
        if (!k || i + k > m->count) return 0;   /* (the runs must add up to the count, no more) */
        RbnPad p;
        p.buttons = get64(q + 1);
        p.lx = (int16_t)get16(q + 9); p.ly = (int16_t)get16(q + 11); p.rx = (int16_t)get16(q + 13);
        p.ry = (int16_t)get16(q + 15); p.lt = (int16_t)get16(q + 17); p.rt = (int16_t)get16(q + 19);
        while (k--) m->pads[i++] = p;
    }
    return i == m->count;
}

size_t rbn_hello_build(uint8_t *b, size_t cap, const RbnHello *h) {
    if (cap < RBN_HELLO_BYTES || h->player < -1 || h->player >= RBN_PLAYERS) return 0;   /* (-1: a joiner with no number yet) */
    memset(b, 0, RBN_HELLO_BYTES);
    b[0] = RBN_MSG_HELLO; b[1] = h->player < 0 ? 0xFF : (uint8_t)h->player; b[2] = (uint8_t)h->version; b[3] = (uint8_t)(h->agreed != 0);
    b[4] = (uint8_t)(h->input_delay < 0 ? 0 : h->input_delay > 255 ? 255 : h->input_delay);
    b[5] = (uint8_t)(h->players < 0 ? 0 : h->players > RBN_PLAYERS ? RBN_PLAYERS : h->players);
    b[6] = (uint8_t)(h->assign < 0 ? 0 : h->assign > RBN_PLAYERS ? 0 : h->assign);
    put64(b + 8, h->seed); put64(b + 16, h->t); put64(b + 24, h->echo); put32(b + 32, h->hold);
    return RBN_HELLO_BYTES;
}
int rbn_delay_for_rtt(double rtt_ms) {
    /* one more frame each 30 ms, rounded: at 46 ms and delay 1 the internet test rolled back every 4 frames (the
     * other player snapping about); at 48 ms and delay 2 still every 6 on the host, the joiner's pads late over its
     * Wi-Fi (the round trip, the least of 30, can't show one way being slower): 45-75 ms now gets 3. Under 15 ms (a
     * home network) stays 1 */
    int d = 1 + (int)(rtt_ms > 0 ? (rtt_ms + 15) / 30 : 0);
    return d > 5 ? 5 : d;
}
int rbn_hello_parse(const uint8_t *b, size_t len, RbnHello *h) {
    /* (a version 1 hello, 16 bytes, is read far enough to say which version the other game speaks) */
    if ((len != RBN_HELLO_BYTES && len != 16) || b[0] != RBN_MSG_HELLO || (b[1] >= RBN_PLAYERS && b[1] != 0xFF)) return 0;
    memset(h, 0, sizeof *h);
    h->player = b[1] == 0xFF ? -1 : b[1]; h->version = b[2]; h->agreed = b[3]; h->seed = get64(b + 8);
    if (len == RBN_HELLO_BYTES) {
        h->input_delay = b[4]; h->players = b[5]; h->assign = b[6] <= RBN_PLAYERS ? b[6] : 0;
        h->t = get64(b + 16); h->echo = get64(b + 24); h->hold = get32(b + 32);
    }
    return 1;
}

size_t rbn_choice_build(uint8_t *b, size_t cap, const RbnChoice *c) {
    if (cap < RBN_CHOICE_BYTES || c->player < 0 || c->player >= RBN_PLAYERS) return 0;
    memset(b, 0, RBN_CHOICE_BYTES);
    b[0] = RBN_MSG_CHOICE; b[1] = (uint8_t)c->player; b[2] = (uint8_t)c->colour;
    b[3] = (uint8_t)((c->has_stage != 0) | (c->ask != 0) << 1 | (c->has_tag != 0) << 2 | (c->gc != 0) << 3); b[4] = (uint8_t)c->hazards;
    put64(b + 8, c->chara);
    memcpy(b + 16, c->stage, 0x20);
    if (c->has_tag) memcpy(b + 48, c->tag, RBN_TAG_BYTES);
    return RBN_CHOICE_BYTES;
}
int rbn_choice_parse(const uint8_t *b, size_t len, RbnChoice *c) {
    if (len != RBN_CHOICE_BYTES || b[0] != RBN_MSG_CHOICE || b[1] >= RBN_PLAYERS) return 0;
    c->player = b[1]; c->colour = b[2]; c->has_stage = b[3] & 1; c->ask = b[3] >> 1 & 1; c->has_tag = b[3] >> 2 & 1; c->gc = b[3] >> 3 & 1; c->hazards = b[4];
    c->chara = get64(b + 8);
    memcpy(c->stage, b + 16, 0x20);
    memcpy(c->tag, b + 48, RBN_TAG_BYTES);
    return 1;
}

size_t rbn_rules_build(uint8_t *b, size_t cap, const RbnRules *r) {
    if (r->player < 0 || r->player >= RBN_PLAYERS || r->len < 0 || r->len > RBN_RULES_CHUNK || r->off < 0 || r->total < 0 ||
        r->total > RBN_RULES_MAX || r->off + r->len > r->total || cap < (size_t)(RBN_RULES_HEAD + r->len)) return 0;
    memset(b, 0, RBN_RULES_HEAD);
    b[0] = RBN_MSG_RULES; b[1] = (uint8_t)r->player; b[2] = (uint8_t)(r->ask != 0);
    put32(b + 4, r->id); put16(b + 8, (uint16_t)r->total); put16(b + 10, (uint16_t)r->off); put16(b + 12, (uint16_t)r->len);
    memcpy(b + RBN_RULES_HEAD, r->data, (size_t)r->len);
    return RBN_RULES_HEAD + (size_t)r->len;
}
int rbn_rules_parse(const uint8_t *b, size_t len, RbnRules *r) {
    if (len < RBN_RULES_HEAD || b[0] != RBN_MSG_RULES || b[1] >= RBN_PLAYERS) return 0;
    r->player = b[1]; r->ask = b[2] & 1; r->id = get32(b + 4);
    r->total = get16(b + 8); r->off = get16(b + 10); r->len = get16(b + 12);
    if (r->len > RBN_RULES_CHUNK || r->total > RBN_RULES_MAX || r->off + r->len > r->total || len != RBN_RULES_HEAD + (size_t)r->len) return 0;
    memcpy(r->data, b + RBN_RULES_HEAD, (size_t)r->len);
    return 1;
}

size_t rbn_ready_build(uint8_t *b, size_t cap, const RbnReady *r) {
    if (cap < RBN_READY_BYTES || r->player < 0 || r->player >= RBN_PLAYERS) return 0;
    b[0] = RBN_MSG_READY; b[1] = (uint8_t)r->player; b[2] = (uint8_t)(r->all != 0); b[3] = 0;
    return RBN_READY_BYTES;
}
int rbn_ready_parse(const uint8_t *b, size_t len, RbnReady *r) {
    if (len != RBN_READY_BYTES || b[0] != RBN_MSG_READY || b[1] >= RBN_PLAYERS) return 0;
    r->player = b[1]; r->all = b[2] & 1;
    return 1;
}

size_t rbn_menu_build(uint8_t *b, size_t cap, const RbnMenu *m) {
    if (cap < RBN_MENU_BYTES || m->player < 0 || m->player >= RBN_PLAYERS) return 0;
    b[0] = RBN_MSG_MENU; b[1] = (uint8_t)m->player; b[2] = (uint8_t)(m->buttons & 3); b[3] = 0;
    return RBN_MENU_BYTES;
}
int rbn_menu_parse(const uint8_t *b, size_t len, RbnMenu *m) {
    if (len != RBN_MENU_BYTES || b[0] != RBN_MSG_MENU || b[1] >= RBN_PLAYERS) return 0;
    m->player = b[1]; m->buttons = b[2] & 3;
    return 1;
}

size_t rbn_left_build(uint8_t *b, size_t cap, const RbnLeft *l) {
    if (cap < RBN_LEFT_BYTES || l->player < 0 || l->player >= RBN_PLAYERS) return 0;
    b[0] = RBN_MSG_LEFT; b[1] = (uint8_t)l->player; b[2] = b[3] = 0; put32(b + 4, l->frame);
    return RBN_LEFT_BYTES;
}
int rbn_left_parse(const uint8_t *b, size_t len, RbnLeft *l) {
    if (len != RBN_LEFT_BYTES || b[0] != RBN_MSG_LEFT || b[1] >= RBN_PLAYERS) return 0;
    l->player = b[1]; l->frame = get32(b + 4);
    return 1;
}
size_t rbn_pre_build(uint8_t *b, size_t cap, const RbnPre *p) {
    if (cap < RBN_PRE_BYTES || p->player < 0 || p->player >= RBN_PLAYERS) return 0;
    b[0] = RBN_MSG_PRE; b[1] = (uint8_t)p->player; b[2] = b[3] = 0; put32(b + 4, p->frames);
    return RBN_PRE_BYTES;
}
int rbn_pre_parse(const uint8_t *b, size_t len, RbnPre *p) {
    if (len != RBN_PRE_BYTES || b[0] != RBN_MSG_PRE || b[1] >= RBN_PLAYERS) return 0;
    p->player = b[1]; p->frames = get32(b + 4);
    return 1;
}

/* ---- frame advantage ---- */
static void sync_clear(void) { memset(ndiff, 0, sizeof ndiff); memset(next_diff, 0, sizeof next_diff); }
void rbn_sync_reset(void) { sync_clear(); owed = 0; }
void rbn_sync_sample(int p, int32_t local_adv, int32_t remote_adv) {
    if (p < 0 || p >= RBN_PLAYERS || p == local_player) return;
    diff[p][next_diff[p]] = local_adv - remote_adv;
    next_diff[p] = (next_diff[p] + 1) % SAMPLES;
    if (ndiff[p] < SAMPLES) ndiff[p]++;
}
int rbn_sync_wait(uint32_t frame) {
    /* every 8 frames, once at least 4 samples came since the last wait was paid: half the mean difference, whole
     * frames, at most 8. (M6: it was every 32 frames, at most 4, which could not keep up with a peer at ~20 ms a
     * frame, some 5 frames lost in 32: the game ahead ran into RBN_MAX_BACK, twopc15. rb_net_test "slow": 241
     * stalls that way, 3 now, the normal runs about the same.) */
    /* (M7: per player; the wait is for the one this game is furthest ahead of, once every other player has given
     * at least 4 samples: a game must not run ahead of the slowest, nor wait on a player not heard from yet) */
    if (frame % 8 == 0 && !owed) {
        int w = 0, ready = 1, any = 0;
        for (int p = 0; p < nplayers; p++) {
            if (p == local_player || remote[p].gone) continue;   /* (gone: no samples come) */
            if (ndiff[p] < 4) { ready = 0; break; }
            int64_t sum = 0;
            for (int i = 0; i < ndiff[p]; i++) sum += diff[p][i];
            int wp = (int)(sum / ndiff[p] / 2);
            if (!any || wp > w) w = wp;
            any = 1;
        }
        if (ready && any) {
            owed = w < 0 ? 0 : w > 8 ? 8 : w;
            sync_clear();   /* (samples from before the wait would ask for it again) */
        }
    }
    if (owed && frame % 4 == 0) {   /* (spread: one frame in four) */
        if (!--owed) sync_clear();   /* (paid: samples taken while paying would ask for part of it again) */
        return 1;
    }
    return 0;
}
