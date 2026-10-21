/* The application's environment, natively: operating environment (nn::oe), time, the user
 * account, parental controls, add-on content, network status and error reporting.
 *
 * On the console each of these is a session with a system service. Natively the game runs
 * docked (TV output), in focus, for one local user, with the network unavailable - so
 * online features take their offline paths. */
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "host.h"
#include "net.h"

#define CREATE_SYSTEM_EVENT "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb"
#define SIGNAL_SYSTEM_EVENT "_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE"
#define R_UNAVAILABLE RESULT(124, 300)   /* "not available": any failure takes the offline path */

/* ---- nn::oe ---------------------------------------------------------------------- */
HLE(_ZN2nn2oe10InitializeEv) {
    /* The notification event is a static the SDK hands out; it must be a live event that
     * simply never fires (no focus changes, no sleep, no docking changes). */
    u64 event = rt_callsym(c, "_ZN2nn2oe27GetNotificationMessageEventEv", 0);
    rt_callsym(c, CREATE_SYSTEM_EVENT, 3, event, 1 /* auto clear */, 0);
}
HLE(_ZN2nn2oe16GetOperationModeEv) { c->x[0] = 1; }                 /* docked: TV output */
HLE(_ZN2nn2oe20GetCurrentFocusStateEv) { c->x[0] = 1; }             /* in focus */
HLE(_ZN2nn2oe25TryPopNotificationMessageEPj) { c->x[0] = 0; }       /* no messages */
HLE(_ZN2nn2oe18GetDesiredLanguageEv) {                             /* a LanguageCode, by value */
    u64 code = 0;
    memcpy(&code, "en-US", 5);
    c->x[0] = code;
}
HLE(_ZN2nn2oe25GetProgramTotalActiveTimeEv) { c->x[0] = rt_now_ns(); }
HLE(_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_) { *P(f32, c->x[0]) = 1.0f; *P(f32, c->x[1]) = 1.0f; }
HLE(_ZN2nn2oe28RequestToRelaunchApplicationEv) { rt_fatal("the game asked to be relaunched"); }
/* Requests that only matter to the console's system software. */
HLE(_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE) { (void)c; }
HLE(_ZN2nn2oe16DisableRecordingEv) { (void)c; }
HLE(_ZN2nn2oe16EnableScreenShotEv) { (void)c; }
HLE(_ZN2nn2oe17DisableScreenShotEv) { (void)c; }
HLE(_ZN2nn2oe17FinishStartupLogoEv) { (void)c; }
HLE(_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE) { (void)c; }
HLE(_ZN2nn2oe20SwitchLcdBacklightOnEv) { (void)c; }
HLE(_ZN2nn2oe21SwitchLcdBacklightOffEv) { (void)c; }
HLE(_ZN2nn2oe24SetExpectedVolumeBalanceEff) { (void)c; }
HLE(_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi) { (void)c; }
HLE(_ZN2nn2oe27SetTvPowerStateMatchingModeENS0_24TvPowerStateMatchingModeE) { (void)c; }
HLE(_ZN2nn2oe27EndAutoSleepDisabledSectionEv) { (void)c; }
HLE(_ZN2nn2oe29BeginAutoSleepDisabledSectionEv) { (void)c; }
HLE(_ZN2nn2oe31EnterExitRequestHandlingSectionEv) { (void)c; }
HLE(_ZN2nn2oe31LeaveExitRequestHandlingSectionEv) { (void)c; }
HLE(_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb) { (void)c; }

/* ---- nn::time -------------------------------------------------------------------- */
static const u8 STEADY_SOURCE[16] = {0x53, 0x53, 0x42, 0x55, 0x2D, 0x6E, 0x61, 0x74, 0x69, 0x76, 0x65, 0x2D, 0x63, 0x6C, 0x6B, 0x01};

static i64 steady_seconds(void) { return 1000000 + (i64)(rt_now_ns() / 1000000000ull); }

static void steady_point(u64 out) {   /* SteadyClockTimePoint: { s64 seconds; Uuid source } */
    st64(out, (u64)steady_seconds());
    memcpy(P(void, out + 8), STEADY_SOURCE, 16);
}

/* CalendarTime { s16 year; s8 month, day, hour, minute, second; } and
 * CalendarAdditionalInfo { s32 day of week; s32 day of year; char zone[8]; s32 dst; s32 utc offset; } */
static void calendar(u64 out, u64 info, i64 posix) {
    time_t t = (time_t)posix;
    struct tm lt;
    localtime_s(&lt, &t);
    st16(out, (u16)(lt.tm_year + 1900));
    st8(out + 2, (u8)(lt.tm_mon + 1)); st8(out + 3, (u8)lt.tm_mday);
    st8(out + 4, (u8)lt.tm_hour); st8(out + 5, (u8)lt.tm_min); st8(out + 6, (u8)lt.tm_sec); st8(out + 7, 0);
    if (info) {
        long offset;
        _get_timezone(&offset);
        memset(P(void, info), 0, 0x18);
        st32(info, (u32)lt.tm_wday);
        st32(info + 4, (u32)lt.tm_yday);
        memcpy(P(void, info + 8), "LOCAL", 5);
        st32(info + 16, lt.tm_isdst > 0);
        st32(info + 20, (u32)(-offset + (lt.tm_isdst > 0 ? 3600 : 0)));
    }
}

HLE(_ZN2nn4time10InitializeEv) { c->x[0] = 0; }
HLE(_ZN2nn4time8FinalizeEv) { c->x[0] = 0; }
HLE(_ZN2nn4time13IsInitializedEv) { c->x[0] = 1; }
HLE(_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE) { st64(c->x[0], (u64)time(NULL)); c->x[0] = 0; }
HLE(_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE) { st64(c->x[0], (u64)time(NULL)); c->x[0] = 0; }
/* The std::chrono-style clock: its time_point is the seconds alone, an 8-byte value returned in x0
 * (the SDK keeps the whole SteadyClockTimePoint in its own frame). Callers leave x8 as it was, so
 * writing a point there overwrote whatever x8 pointed at (the Spirit Board's data: a crash). */
HLE(_ZN2nn4time19StandardSteadyClock3nowEv) { c->x[0] = (u64)steady_seconds(); }
HLE(_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE) {
    calendar(c->x[0], c->x[1], (i64)ld64(c->x[2]));
    c->x[0] = 0;
}
/* Time-zone rules come from system data that is not needed: the host's zone is used. */
HLE(_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE) { c->x[0] = 0; }
HLE(_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE) {
    calendar(c->x[0], c->x[1], (i64)ld64(c->x[2]));
    c->x[0] = 0;
}
HLE(_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeE) {   /* (count*, out*, capacity, local) */
    u64 cal = c->x[3];
    struct tm lt = {0};
    lt.tm_year = (i16)ld16(cal) - 1900; lt.tm_mon = (i8)ld8(cal + 2) - 1; lt.tm_mday = (i8)ld8(cal + 3);
    lt.tm_hour = (i8)ld8(cal + 4); lt.tm_min = (i8)ld8(cal + 5); lt.tm_sec = (i8)ld8(cal + 6); lt.tm_isdst = -1;
    if (c->x[2] >= 1) st64(c->x[1], (u64)mktime(&lt));
    st32(c->x[0], c->x[2] >= 1);
    c->x[0] = 0;
}
/* ClockSnapshot, 0xD0 bytes: two clock contexts { s64 offset; SteadyClockTimePoint }, user and
 * network times, their calendar forms, a steady time point and the location name. */
HLE(_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_) {
    u64 s = c->x[0];
    i64 now = (i64)time(NULL), steady = steady_seconds();
    memset(P(void, s), 0, 0xD0);
    for (u64 ctx = s; ctx <= s + 0x20; ctx += 0x20) {
        st64(ctx, (u64)(now - steady));
        steady_point(ctx + 8);
    }
    st64(s + 0x40, (u64)now);
    st64(s + 0x48, (u64)now);
    calendar(s + 0x50, s + 0x60, now);
    calendar(s + 0x58, s + 0x78, now);
    steady_point(s + 0x90);
    memcpy(P(void, s + 0xA8), "UTC", 3);
    st8(s + 0xCC, 1);
    c->x[0] = 0;
}
HLE(_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_) {   /* -> bool */
    i64 a = (i64)ld64(c->x[1] + 0x90), b = (i64)ld64(c->x[2] + 0x90);
    st64(c->x[0], (u64)((b - a) * 1000000000ll));
    c->x[0] = 1;
}

/* ---- nn::account: one local user, no network service account ---------------------- */
/* The user's id. With netplay each install has one of its own (a LAN session tells players
 * apart by it), kept in netplay_id.txt beside the save folder; the save data stays where the
 * console user's always was (account_save_id). */
static const u64 SAVE_USER[2] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};

static const u64 *user_id(void) {
    static u64 id[2];
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (!pending) return id;
    id[0] = SAVE_USER[0];
    id[1] = SAVE_USER[1];
    if (net_configured()) {
        char path[600];
        const char *root = getenv("SSBU_DATA_ROOT");
        if (root) snprintf(path, sizeof path, "%s/netplay_id.txt", root);
        else snprintf(path, sizeof path, "%s/../../netplay_id.txt", rt_data_dir);
        FILE *f = fopen(path, "r");
        unsigned long long a = 0, b = 0;
        if (!f || fscanf(f, "%llx %llx", &a, &b) != 2 || (!a && !b)) {
            unsigned int r[4];
            for (int i = 0; i < 4; i++) rand_s(&r[i]);
            a = (u64)r[0] << 32 | r[1];
            b = (u64)r[2] << 32 | r[3];
            if (f) fclose(f);
            f = fopen(path, "w");
            if (f) fprintf(f, "%016llx %016llx\n", a, b);
        }
        if (f) fclose(f);
        id[0] = a;
        id[1] = b;
    }
    InitOnceComplete(&once, 0, NULL);
    return id;
}
#define USER (user_id())

void account_save_id(u64 uid[2]) {   /* (hle_fs.c) the id a user's save folder is named by */
    if (uid[0] == USER[0] && uid[1] == USER[1]) { uid[0] = SAVE_USER[0]; uid[1] = SAVE_USER[1]; }
}

HLE(_ZN2nn7account10InitializeEv) { (void)c; }
HLE(_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi) {
    if ((i32)c->x[2] > 0) { st64(c->x[1], USER[0]); st64(c->x[1] + 8, USER[1]); }
    st32(c->x[0], (i32)c->x[2] > 0);
    c->x[0] = 0;
}
HLE(_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE) {
    st8(c->x[0], ld64(c->x[1]) == USER[0] && ld64(c->x[1] + 8) == USER[1]);
    c->x[0] = 0;
}
HLE(_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE) {   /* -> bool; UserHandle { u64[3] } */
    st64(c->x[0], USER[0]); st64(c->x[0] + 8, USER[1]); st64(c->x[0] + 16, 1);
    c->x[0] = 1;
}
HLE(_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE) {
    st64(c->x[0], ld64(c->x[1])); st64(c->x[0] + 8, ld64(c->x[1] + 8));
    c->x[0] = 0;
}
HLE(_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE) {
    st64(c->x[0], USER[0]); st64(c->x[0] + 8, USER[1]);
    c->x[0] = 0;
}
HLE(_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE) {   /* Nickname { char[33] } */
    memset(P(void, c->x[0]), 0, 33);
    const char *name = net_configured() ? net_player_name() : "Player";
    memcpy(P(void, c->x[0]), name, strlen(name) < 32 ? strlen(name) : 32);
    c->x[0] = 0;
}
/* The local user is always present and "open" (selected for the game). */
HLE(_ZN2nn7account12GetUserCountEPi) { st32(c->x[0], 1); c->x[0] = 0; }
HLE(_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi) {
    if ((i32)c->x[2] > 0) { st64(c->x[1], USER[0]); st64(c->x[1] + 8, USER[1]); }
    st32(c->x[0], (i32)c->x[2] > 0);
    c->x[0] = 0;
}
HLE(_ZN2nn7account17GetLastOpenedUserEPNS0_3UidE) { st64(c->x[0], USER[0]); st64(c->x[0] + 8, USER[1]); c->x[0] = 0; }HLE(_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE) {
    st64(c->x[0], ld64(c->x[1])); st64(c->x[0] + 8, ld64(c->x[1] + 8)); st64(c->x[0] + 16, 1);
    c->x[0] = 0;
}
HLE(_ZN2nn7account9CloseUserERKNS0_10UserHandleE) { (void)c; }
HLE(_ZN2nn7account34IsUserRegistrationRequestPermittedEPb) { st8(c->x[0], 0); c->x[0] = 0; }
HLE(_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE) { st64(c->x[0], 0); c->x[0] = R_UNAVAILABLE; }
/* The network service account, for the game's Online mode (runtime/hle_online.c): with netplay
 * configured the user has one, named by the netplay id, and its id token - which only the NEX
 * server of tools/nex reads - carries that id and the player's name. Offline there is none. */
static u64 nsa_id(void) { u64 id = USER[0] ^ USER[1]; return id ? id : 1; }
HLE(_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE) { st8(c->x[0], net_configured() != 0); c->x[0] = 0; }
HLE(_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE) { c->x[0] = net_configured() ? 0 : R_UNAVAILABLE; }
HLE(_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE) {
    if (!net_configured()) { c->x[0] = R_UNAVAILABLE; return; }
    st64(c->x[0], nsa_id());
    c->x[0] = 0;
}
/* CachedNintendoAccountInfo { char birthday[16]; char gender[8]; char country[4]; ... } (its
 * getters read these): Online's matchmaking asks for the country. SSBU_ONLINE_COUNTRY (two
 * letters) picks it; US otherwise. */
HLE(_ZN2nn7account29LoadCachedNintendoAccountInfoEPNS0_25CachedNintendoAccountInfoERKNS0_10UserHandleEPvm) {
    if (!net_configured()) { c->x[0] = R_UNAVAILABLE; return; }
    char *info = P(char, c->x[0]);
    const char *country = getenv("SSBU_ONLINE_COUNTRY");
    memset(info, 0, 36);
    memcpy(info, "1990-01-01", 10);
    memcpy(info + 16, "unknown", 7);
    memcpy(info + 32, country && strlen(country) == 2 ? country : "US", 2);
    c->x[0] = 0;
}
HLE(_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE) {   /* (u64 *size, char *buf, u64 capacity, handle) */
    if (!net_configured()) { c->x[0] = R_UNAVAILABLE; return; }
    char token[160];
    int n = snprintf(token, sizeof token, "ssbu-port.1.%016llx.%s", (unsigned long long)nsa_id(), net_player_name());
    if (n < 0 || (u64)n >= sizeof token || (u64)n + 1 > c->x[2]) { c->x[0] = R_UNAVAILABLE; return; }
    memcpy(P(char, c->x[1]), token, (size_t)n + 1);
    st64(c->x[0], (u64)n);
    c->x[0] = 0;
}
HLE(_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE) { c->x[0] = net_configured() ? 0 : R_UNAVAILABLE; }
/* An AsyncContext here never has an operation behind it: it is always finished - successfully
 * when netplay (and so the token cache above) is there. */
HLE(_ZN2nn7account12AsyncContext7HasDoneEPb) { st8(c->x[1], 1); c->x[0] = 0; }
HLE(_ZN2nn7account12AsyncContext9GetResultEv) { c->x[0] = net_configured() ? 0 : R_UNAVAILABLE; }
HLE(_ZN2nn7account12AsyncContext6CancelEv) { c->x[0] = 0; }
HLE(_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE) {
    u64 event = c->x[1];
    rt_callsym(c, CREATE_SYSTEM_EVENT, 3, event, 0 /* manual clear */, 0);
    rt_callsym(c, SIGNAL_SYSTEM_EVENT, 1, event);
    c->x[0] = 0;
}

/* ---- parental controls, add-on content, play statistics -------------------------- */
HLE(_ZN2nn4pctl27CheckStereoVisionPermissionEb) { c->x[0] = 1; }
HLE(_ZN2nn4pctl32CheckFreeCommunicationPermissionEb) { c->x[0] = 1; }

/* Add-on content: the installed DLC's indices (title id - the application's AOC base id), one
 * per line of <data dir>/aoc.cfg, which tools/nca.py aoc-cfg writes from the installed DLC.
 * The game only counts and lists them: Smash's DLC data ships in the game's own RomFS, and an
 * add-on content package just unlocks it. No aoc.cfg means no DLC. */
static i32 aoc_index[512], aoc_count;

static void aoc_load(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (!pending) return;
    char path[1024], line[256];
    snprintf(path, sizeof path, "%s/aoc.cfg", rt_data_dir);
    FILE *f = fopen(path, "r");
    while (f && aoc_count < (i32)(sizeof aoc_index / sizeof *aoc_index) && fgets(line, sizeof line, f)) {
        int index;
        if (line[0] != '#' && sscanf(line, "%d", &index) == 1 && index > 0) aoc_index[aoc_count++] = index;
    }
    if (f) fclose(f);
    if (aoc_count) fprintf(stderr, "[aoc] %d add-on contents installed (%s)\n", aoc_count, path);
    InitOnceComplete(&once, 0, NULL);
}

HLE(_ZN2nn3aoc17CountAddOnContentEv) { aoc_load(); c->x[0] = (u32)aoc_count; }
HLE(_ZN2nn3aoc16ListAddOnContentEPiii) {   /* (int *indices, int offset, int count) -> count written */
    aoc_load();
    i32 offset = (i32)c->x[1], count = (i32)c->x[2], n = 0;
    for (i32 i = offset; i >= 0 && i < aoc_count && n < count; i++) st32(c->x[0] + 4ull * n++, (u32)aoc_index[i]);
    c->x[0] = (u32)n;
}
HLE(_ZN2nn3pdm30QueryApplicationPlayStatisticsEPNS0_25ApplicationPlayStatisticsEPKNS_13ApplicationIdEi) {
    for (i32 i = 0; i < (i32)c->x[2]; i++) {   /* { ApplicationId; TimeSpan total; s64 launches } */
        st64(c->x[0] + 24ull * i, ld64(c->x[1] + 8ull * i));
        st64(c->x[0] + 24ull * i + 8, 0);
        st64(c->x[0] + 24ull * i + 16, 0);
    }
    c->x[0] = (u32)c->x[2];
}

/* ---- name service (network status: hle_net.c) ------------------------------------ */
HLE(_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_) { memcpy(P(void, c->x[0]), P(void, c->x[1]), 256); c->x[0] = 0; }

/* ---- errors ---------------------------------------------------------------------- */
HLE(_ZN2nn3err9ShowErrorENS0_9ErrorCodeE) {   /* ErrorCode { u32 category; u32 number } in x0 */
    fprintf(stderr, "[game] error shown: %04u-%04u\n", (u32)(c->x[0] & 0xFFFFFFFF), (u32)(c->x[0] >> 32));
    if (net_log() > 1) rt_backtrace();
}
HLE(_ZN2nn3err9ShowErrorENS_6ResultE) {
    fprintf(stderr, "[game] error shown: result %#x (module %u, description %u)\n", (u32)c->x[0],
            (u32)c->x[0] & 0x1FF, ((u32)c->x[0] >> 9) & 0x1FFF);
}
HLE(_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i) {
    rt_fatal("the game aborted: %s in %s (%s:%d)", c->x[0] ? P(char, c->x[0]) : "?", c->x[1] ? P(char, c->x[1]) : "?",
             c->x[2] ? P(char, c->x[2]) : "?", (int)c->x[3]);
}
HLE(_ZN2nn6detail21UnexpectedDefaultImplEPKcS2_i) {
    rt_fatal("the game reached an unexpected default case in %s (%s:%d)", c->x[0] ? P(char, c->x[0]) : "?",
             c->x[1] ? P(char, c->x[1]) : "?", (int)c->x[2]);
}
