/* setjmp/longjmp across recompiled code.
 *
 * A guest setjmp returns a second time when a longjmp names its buffer - into the middle of
 * the function that called it. Recompiled functions are C functions, so the recompiler turns
 * each setjmp call into a host setjmp in that very function (tools/lift.py), after
 * rt_setjmp records the guest registers that setjmp preserves: x19-x29, sp and the low
 * halves of v8-v15 (here: all of them). The native longjmp puts those back, sets the return
 * value and longjmps the host to the recorded frame. Records are keyed by the guest buffer. */
#include "host.h"

typedef struct Record {
    u64 guest;
    jmp_buf host;
    u64 x[11], sp;
    V128 v[8];
    struct Record *next;
} Record;

#define BUCKETS 1024
static Record *table[BUCKETS];
static SRWLOCK lock = SRWLOCK_INIT;

static Record *record(u64 guest, int create) {
    u32 b = (u32)((guest >> 3) * 2654435761u) % BUCKETS;
    AcquireSRWLockShared(&lock);
    Record *r = table[b];
    while (r && r->guest != guest) r = r->next;
    ReleaseSRWLockShared(&lock);
    if (r || !create) return r;
    r = _aligned_malloc(sizeof *r, 16);
    memset(r, 0, sizeof *r);
    r->guest = guest;
    AcquireSRWLockExclusive(&lock);
    r->next = table[b];
    table[b] = r;
    ReleaseSRWLockExclusive(&lock);
    return r;
}

jmp_buf *rt_setjmp(Cpu *c) {
    Record *r = record(c->x[0], 1);
    memcpy(r->x, &c->x[19], sizeof r->x);
    r->sp = c->sp;
    memcpy(r->v, &c->v[8], sizeof r->v);
    return &r->host;
}

static void guest_longjmp(Cpu *c) {
    Record *r = record(c->x[0], 0);
    if (!r) rt_fatal("longjmp to a buffer (%#llx) no setjmp filled", c->x[0]);
    int value = (int)(u32)c->x[1];
    if (!value) value = 1;
    memcpy(&c->x[19], r->x, sizeof r->x);
    c->sp = r->sp;
    memcpy(&c->v[8], r->v, sizeof r->v);
    c->x[0] = (u32)value;
    RT_LONGJMP(r->host, value);   /* (the host setjmp's own pair: recomp.h) */
}

/* (rollback build, M2) Every record's contents for a savestate (the host jmp_buf names a frame of
 * a host stack the savestate puts back too). Records made since a save stay: a guest buffer is
 * set again before it is jumped to. */
typedef struct RSave { Record *r; Record v; } RSave;
void *setjmp_rb_save(u32 *count) {
    u32 n = 0;
    AcquireSRWLockShared(&lock);
    for (u32 b = 0; b < BUCKETS; b++) for (Record *r = table[b]; r; r = r->next) n++;
    RSave *s = _aligned_malloc(sizeof *s * (n + 1), 16);
    u32 i = 0;
    for (u32 b = 0; b < BUCKETS; b++) for (Record *r = table[b]; r; r = r->next) { s[i].r = r; s[i].v = *r; i++; }
    ReleaseSRWLockShared(&lock);
    s[n].r = NULL;
    *count = n;
    return s;
}

void setjmp_rb_load(const void *p) {
    AcquireSRWLockExclusive(&lock);
    for (const RSave *s = p; s->r; s++) {
        Record *next = s->r->next;
        *s->r = s->v;
        s->r->next = next;
    }
    ReleaseSRWLockExclusive(&lock);
}

void setjmp_rb_free(void *p) { _aligned_free(p); }

HLE(longjmp) { guest_longjmp(c); }
HLE(_longjmp) { guest_longjmp(c); }
