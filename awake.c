/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Awake: keep-screen-awake feature (ported from the Awake.exe design).
 *
 * ARCHITECTURE RULE (do not violate):
 *   This module MUST stay on the main program side (compiled into
 *   AltSnap.exe, included from altsnap.c). NEVER move or include it
 *   from hooks.c: SetThreadExecutionState() is a THREAD-level API and
 *   hooks.c is compiled into hooks.dll which gets injected into every
 *   process on the system.
 *
 * Design (port of Awake.exe v1.1 "interval keep-awake"):
 *   - Only Win32 call: kernel32!SetThreadExecutionState, resolved
 *     dynamically for NT4 compatibility.
 *     ON  = ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED
 *     OFF = ES_CONTINUOUS (restores the power plan control)
 *   - A resident 10 s schedule timer only detects state flips
 *     (ES_CONTINUOUS makes one call effective until the next one),
 *     a 250 ms countdown timer handles the countdown mode.
 *   - Pause (CMD_TOGGLE / UnhookSystem) is orthogonal to Awake by
 *     design: disabling AltSnap hooks never stops keeping the screen
 *     awake; both have separate tray switches.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef AWAKE_C_A10E
#define AWAKE_C_A10E

#define AWAKE_MAX_RANGES       16    // max concurrent time ranges
#define AWAKE_RANGES_STR_SIZE  (AWAKE_MAX_RANGES*14+2) // "hh:mm-hh:mm;" each
#define AWAKE_SCHED_TIMER_ID   0xA10E // 10 s schedule polling timer
#define AWAKE_CD_TIMER_ID      0xA10F // 250 ms countdown timer

// ES_ flag combinations
#define AWAKE_ES_OFF 0x80000000u                            // ES_CONTINUOUS
#define AWAKE_ES_ON  (0x80000000u|0x00000002u|0x00000001u)  // +SYSTEM+DISPLAY

// Modes (mirror the Awake.exe _mode field)
#define AWAKE_MODE_CONT      0
#define AWAKE_MODE_COUNTDOWN 1
#define AWAKE_MODE_SCHEDULE  2

typedef struct {
    int sh, sm, eh, em; // start hour/min, end hour/min
} awake_range_t;

// Persisted runtime state ([Awake] ini section)
static int awake_enabled = 0;  // feature on/off
static int awake_mode = AWAKE_MODE_SCHEDULE; // 0=cont, 1=countdown, 2=schedule
static int awake_cdmin = 30;   // countdown duration, minutes 1..1440
static awake_range_t awake_ranges[AWAKE_MAX_RANGES];
static int awake_nranges = 0;

// Transient state
static int awake_screen_awake = 0; // current ES_ state flip-flop
static DWORD awake_countdown_start = 0;

// SetThreadExecutionState() resolved dynamically for NT4 compatibility.
typedef UINT (WINAPI *AwakeSetTES_t)(DWORD);
static AwakeSetTES_t AwakeSetTES = NULL;

static void AwakeResolveAPI(void)
{
    if (!AwakeSetTES)
        AwakeSetTES = (AwakeSetTES_t)GetProcAddress(
            GetModuleHandle(TEXT("kernel32.dll")), "SetThreadExecutionState");
}

// Apply (or revert) the display/system keep-awake requirement.
static void AwakeSetExecutionState(int on)
{
    AwakeResolveAPI();
    if (AwakeSetTES)
        AwakeSetTES(on ? AWAKE_ES_ON : AWAKE_ES_OFF);
    awake_screen_awake = on ? 1 : 0;
}

/////////////////////////////////////////////////////////////////////////////
// Range testing (port of Awake.exe IsInSchedule/IsInAnySchedule)
// t is minutes since midnight.

static int AwakeIsInSchedule(int t, const awake_range_t *r)
{
    int s = r->sh*60 + r->sm;
    int e = r->eh*60 + r->em;
    if (s < e) return t >= s && t < e;  // normal range [s, e)
    if (s > e) return t >= s || t < e;  // crossing midnight: [s,24h) U [0,e)
    return 0;                           // degenerate (rejected at add time)
}

static int AwakeIsInAnySchedule(int t)
{
    int i;
    for (i = 0; i < awake_nranges; i++)
        if (AwakeIsInSchedule(t, &awake_ranges[i]))
            return 1;
    return 0;
}

static int AwakeNowMinutes(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wHour*60 + st.wMinute;
}

/////////////////////////////////////////////////////////////////////////////
// Range list management helpers (used by the config page)

// Returns 1 on success, 0 if the list is full.
static int AwakeAddRange(int sh, int sm, int eh, int em)
{
    awake_range_t *r;
    if (awake_nranges >= AWAKE_MAX_RANGES)
        return 0;
    r = &awake_ranges[awake_nranges++];
    r->sh = sh; r->sm = sm; r->eh = eh; r->em = em;
    return 1;
}

static void AwakeDeleteRange(int idx)
{
    if (idx < 0 || idx >= awake_nranges)
        return;
    while (++idx < awake_nranges)
        awake_ranges[idx-1] = awake_ranges[idx];
    awake_nranges--;
}

static void AwakeReplaceRange(int idx, int sh, int sm, int eh, int em)
{
    awake_range_t *r;
    if (idx < 0 || idx >= awake_nranges)
        return;
    r = &awake_ranges[idx];
    r->sh = sh; r->sm = sm; r->eh = eh; r->em = em;
}

/////////////////////////////////////////////////////////////////////////////
// Config persistence: [Awake] section of the ini file

static void AwakeSerializeRanges(TCHAR *out, size_t cap)
{
    size_t o = 0;
    int i;
    out[0] = TEXT('\0');
    for (i = 0; i < awake_nranges && o + 14 < cap; i++) {
        const awake_range_t *r = &awake_ranges[i];
        if (i) out[o++] = TEXT(';');
        o += wsprintf(out+o, TEXT("%02d:%02d-%02d:%02d"), r->sh, r->sm, r->eh, r->em);
    }
    out[o] = TEXT('\0');
}

// Strict parser for "sh:sm-eh:em;..." lines: invalid entries are dropped.
static void AwakeParseRanges(const TCHAR *p)
{
    awake_nranges = 0;
    if (!p) return;
    while (*p && awake_nranges < AWAKE_MAX_RANGES) {
        int v[4];
        int ok = 1, i;
        for (i = 0; i < 4 && ok; i++) {
            int x = 0;
            if (*p < TEXT('0') || *p > TEXT('9')) { ok = 0; break; }
            while (*p >= TEXT('0') && *p <= TEXT('9')) {
                x = x*10 + (*p - TEXT('0'));
                p++;
                if (x > 99) { ok = 0; break; }
            }
            v[i] = x;
            if (i < 3) {
                const TCHAR sep = (i == 0 || i == 2) ? TEXT(':') : TEXT('-');
                if (*p != sep) { ok = 0; break; }
                p++;
            }
        }
        if (ok && *p && *p != TEXT(';'))
            ok = 0;
        if (ok && v[0] <= 23 && v[2] <= 23 && v[1] <= 59 && v[3] <= 59
        && (v[0]*60+v[1]) != (v[2]*60+v[3])) {
            awake_ranges[awake_nranges].sh = v[0];
            awake_ranges[awake_nranges].sm = v[1];
            awake_ranges[awake_nranges].eh = v[2];
            awake_ranges[awake_nranges].em = v[3];
            awake_nranges++;
        }
        // Skip to the next entry separator (also guards parse errors).
        while (*p && *p != TEXT(';')) p++;
        if (*p == TEXT(';')) p++;
    }
}

static void AwakeSaveConfig(void)
{
    TCHAR val[16];
    TCHAR ranges[AWAKE_RANGES_STR_SIZE];
    AwakeSerializeRanges(ranges, ARR_SZ(ranges));
    WritePrivateProfileString(TEXT("Awake"), TEXT("Enabled"),      Uint2lStr(val, (unsigned)awake_enabled), inipath);
    WritePrivateProfileString(TEXT("Awake"), TEXT("Mode"),         Uint2lStr(val, (unsigned)awake_mode), inipath);
    WritePrivateProfileString(TEXT("Awake"), TEXT("CountdownMin"), Uint2lStr(val, (unsigned)awake_cdmin), inipath);
    WritePrivateProfileString(TEXT("Awake"), TEXT("Ranges"),       ranges, inipath);
}

static void AwakeLoadConfig(void)
{
    TCHAR txt[AWAKE_RANGES_STR_SIZE];
    awake_enabled = GetPrivateProfileInt(TEXT("Awake"), TEXT("Enabled"), 0, inipath) ? 1 : 0;
    awake_mode = CLAMP(0, GetPrivateProfileInt(TEXT("Awake"), TEXT("Mode"), AWAKE_MODE_SCHEDULE, inipath), 2);
    awake_cdmin = CLAMP(1, GetPrivateProfileInt(TEXT("Awake"), TEXT("CountdownMin"), 30, inipath), 1440);
    txt[0] = TEXT('\0');
    GetPrivateProfileString(TEXT("Awake"), TEXT("Ranges"), TEXT(""), txt, ARR_SZ(txt), inipath);
    AwakeParseRanges(txt);
}

/////////////////////////////////////////////////////////////////////////////
// Runtime control

// 10 s schedule tick: detects entering/leaving the configured ranges.
// The resident timer keeps ticking; this guard chain short-circuits it.
static void AwakeScheduleTick(void)
{
    if (!awake_enabled || awake_mode != AWAKE_MODE_SCHEDULE || awake_nranges == 0)
        return;
    if (AwakeIsInAnySchedule(AwakeNowMinutes())) {
        if (!awake_screen_awake)
            AwakeSetExecutionState(1); // flip once on entering a range
    } else {
        if (awake_screen_awake)
            AwakeSetExecutionState(0); // flip once on leaving all ranges
    }
}

// 250 ms countdown tick: refresh nothing (tray text is built on demand),
// only stops when the countdown elapses.
static void AwakeCountdownTick(void)
{
    if (!awake_enabled || awake_mode != AWAKE_MODE_COUNTDOWN) {
        KillTimer(g_hwnd, AWAKE_CD_TIMER_ID);
        return;
    }
    if (GetTickCount() - awake_countdown_start >= (DWORD)awake_cdmin * 60000u) {
        AwakeSetExecutionState(0); // countdown elapsed: restore power policy
        KillTimer(g_hwnd, AWAKE_CD_TIMER_ID);
        awake_enabled = 0;
        AwakeSaveConfig();
    }
}

// Reconcile the persistent state with the actual ES_ flags and timers.
static void AwakeUpdateRuntime(void)
{
    if (awake_enabled && awake_mode == AWAKE_MODE_CONT) {
        AwakeSetExecutionState(1); // continuous: keep the flags set
    } else if (awake_enabled && awake_mode == AWAKE_MODE_COUNTDOWN) {
        awake_countdown_start = GetTickCount();
        AwakeSetExecutionState(1);
        SetTimer(g_hwnd, AWAKE_CD_TIMER_ID, 250, NULL);
    } else if (awake_enabled && awake_mode == AWAKE_MODE_SCHEDULE) {
        AwakeScheduleTick(); // check immediately on enable
    } else { // disabled
        AwakeSetExecutionState(0);
        KillTimer(g_hwnd, AWAKE_CD_TIMER_ID);
    }
    // Resident 10 s schedule timer, regardless of mode (cheap guards).
    SetTimer(g_hwnd, AWAKE_SCHED_TIMER_ID, 10000, NULL);
}

// Tray menu switch (CMD_AWAKE_TOGGLE).
static int AwakeEnabled(void)
{
    return awake_enabled;
}

static void AwakeToggle(void)
{
    awake_enabled = !awake_enabled;
    AwakeUpdateRuntime();
    AwakeSaveConfig();
}

// Config page Apply (PSN_APPLY).
static void AwakeApplyFromPage(int enabled, int mode, int cdmin)
{
    awake_enabled = enabled ? 1 : 0;
    awake_mode = CLAMP(0, mode, 2);
    awake_cdmin = CLAMP(1, cdmin, 1440);
    AwakeUpdateRuntime();
    AwakeSaveConfig();
}

// Startup: load config and start the resident timers.
static void AwakeInit(void)
{
    AwakeResolveAPI();
    AwakeLoadConfig();
    AwakeUpdateRuntime();
}

// Shutdown: defensive cleanup only. ES_ state is thread-level and Windows
// reverts it automatically when the process/thread dies anyway.
static void AwakeShutdown(void)
{
    AwakeSetExecutionState(0);
    KillTimer(g_hwnd, AWAKE_SCHED_TIMER_ID);
    KillTimer(g_hwnd, AWAKE_CD_TIMER_ID);
}

#endif // AWAKE_C_A10E
