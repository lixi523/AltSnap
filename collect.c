/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Collect-to-tray: hide a window instead of closing it, and give each
 * collected window its OWN tray icon. Every icon gets a unique guidItem,
 * otherwise Windows groups all icons of one process together (adjacent
 * layout, dragged as one unit) — that grouping is exactly why this module
 * cannot reuse the single AltSnap tray icon.
 *
 * ARCHITECTURE RULE (do not violate):
 *   Tray state, window enumeration and config writes live on the MAIN
 *   PROGRAM side. This file is included from altsnap.c and never from
 *   hooks.c. hooks.c only detects the close action, swallows the input and
 *   posts WM_TRAYCOLLECT.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef COLLECT_C_9F31
#define COLLECT_C_9F31

#define COLLECT_MAX 32
#define COLLECT_REDIR_TIMER 0xC011   /* delayed close of a redirected instance */

typedef struct collect_item {
    HWND  hwnd;
    DWORD pid;
    UINT  id;
    GUID  guid;
    TCHAR exe[MAX_PATH];
} collect_item_t;

static collect_item_t collect_items[COLLECT_MAX];
static int  collect_count = 0;
static UINT collect_nextid = 1;
static UINT collect_cbmsg = 0;   /* Shell_NotifyIcon callback message */

/* Parsed [TrayCollect] config, kept for the single-instance redirect. */
static int   collect_enabled = 0;
static TCHAR collect_processes[2048];
static TCHAR collect_multi[2048];

/* Windows whose new instance must be closed after the collected one is back. */
static HWND collect_pending_close[4];

/* NOTIFYICONDATA V3-compatible layout (Vista+, includes guidItem).
 * Declared locally so the same source builds for both TCHAR variants
 * without depending on the exact NOTIFYICONDATA definition in the SDK. */
typedef struct collect_nid {
    DWORD cbSize;
    HWND  hWnd;
    UINT  uID;
    UINT  uFlags;
    UINT  uCallbackMessage;
    HICON hIcon;
    TCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    TCHAR szInfo[256];
    union {
        UINT uTimeout;
        UINT uVersion;
    } u;
    TCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID  guidItem;
    HICON hBalloonIcon;
} collect_nid_t;

static void CollectSaveConfig(void);
static void CollectReloadConfig(void);
static void CollectLoadWindows(void);

/* Deterministic GUID per (exe, hwnd): Windows remembers the tray position by
 * guidItem, and two icons sharing a GUID would collapse into one slot. */
static GUID CollectGuid(const TCHAR *exe, HWND hwnd)
{
    static const TCHAR prefix[] = TEXT("AltSnap/Collect/");
    const TCHAR *p;
    TCHAR num[32];
    GUID g;
    unsigned long long h1 = 0xCBF29CE484222325ULL; /* FNV-1a 64 offset basis */
    unsigned long long h2 = 0x9E3779B97F4A7C15ULL; /* golden ratio seed */

    mem00(&g, sizeof(g));
    wsprintf(num, TEXT("%u"), (unsigned)(UINT_PTR)hwnd);
    for (p = prefix; *p; p++) {
        h1 = (h1 ^ (unsigned long long)(UINT)*p) * 0x100000001B3ULL;
        h2 = (h2 + (unsigned long long)(UINT)*p) * 0x9E3779B97F4A7C15ULL;
        h2 ^= h2 >> 29;
    }
    for (p = exe; *p; p++) {
        h1 = (h1 ^ (unsigned long long)(UINT)*p) * 0x100000001B3ULL;
        h2 = (h2 + (unsigned long long)(UINT)*p) * 0x9E3779B97F4A7C15ULL;
        h2 ^= h2 >> 29;
    }
    for (p = num; *p; p++) {
        h1 = (h1 ^ (unsigned long long)(UINT)*p) * 0x100000001B3ULL;
        h2 = (h2 + (unsigned long long)(UINT)*p) * 0x9E3779B97F4A7C15ULL;
        h2 ^= h2 >> 29;
    }
    memcpy(&g, &h1, sizeof(h1));
    memcpy((BYTE *)&g + sizeof(h1), &h2, sizeof(h2));
    g.Data3 = (WORD)((g.Data3 & 0x0FFF) | 0x4000);    /* version 4 */
    g.Data4[0] = (BYTE)((g.Data4[0] & 0x3F) | 0x80);  /* RFC 4122 variant */
    return g;
}

static int CollectFindIndex(HWND hwnd)
{
    int i;
    for (i = 0; i < collect_count; i++) {
        if (collect_items[i].hwnd == hwnd)
            return i;
    }
    return -1;
}

/* Cross-process WM_GETTEXT must never block us on a hung application. */
static void CollectWindowText(HWND hwnd, TCHAR *buf, size_t len)
{
    DWORD_PTR res = 0;
    if (!buf || !len) return;
    buf[0] = TEXT('\0');
    if (!SendMessageTimeout(hwnd, WM_GETTEXT, (WPARAM)len, (LPARAM)buf, SMTO_ABORTIFHUNG, 100, &res))
        buf[0] = TEXT('\0');
    buf[len-1] = TEXT('\0');
}

static void CollectFillIdentity(collect_nid_t *nid, int slot)
{
    mem00(nid, sizeof(*nid));
    nid->cbSize = sizeof(*nid);
    nid->hWnd = g_hwnd;
    nid->uID = collect_items[slot].id;
    nid->uCallbackMessage = collect_cbmsg;
    if (WinVer >= VISTA) {
        nid->uFlags = NIF_GUID;
        nid->guidItem = collect_items[slot].guid;
    }
}

static void CollectDropIcon(int slot)
{
    collect_nid_t nid;
    CollectFillIdentity(&nid, slot);
    Shell_NotifyIcon(NIM_DELETE, (PNOTIFYICONDATA)&nid);
}

static void CollectRemoveIndex(int i)
{
    if (i < 0 || i >= collect_count) return;
    CollectDropIcon(i);
    /* Order of the table does not matter to any caller. */
    collect_items[i] = collect_items[--collect_count];
    CollectSaveConfig();
}

/* Show the window again. The tray icon deliberately stays: it represents
 * "this application is collected to tray", not "this window is hidden". */
static void CollectRestoreIndex(int i)
{
    HWND hwnd;
    if (i < 0 || i >= collect_count) return;
    hwnd = collect_items[i].hwnd;
    if (IsWindow(hwnd)) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    }
}

static void CollectWindow(HWND hwnd)
{
    collect_nid_t nid;
    TCHAR exe[MAX_PATH] = {0};
    TCHAR tip[128];
    DWORD pid = 0;
    int i;

    if (!hwnd || !IsWindow(hwnd)) return;
    i = CollectFindIndex(hwnd);
    if (i < 0) {
        if (collect_count >= COLLECT_MAX) return;      /* do not swallow the click */
        if (!GetWindowProgName(hwnd, exe, ARR_SZ(exe))) return;
    }

    ShowWindow(hwnd, SW_HIDE);
    if (IsWindowVisible(hwnd)) {
        /* Could not hide it (elevated target / UIPI): honour the close
         * instead of leaving a window the user can neither see nor close. */
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (i >= 0) return;   /* icon already in the tray: the window is hidden again */

    GetWindowThreadProcessId(hwnd, &pid);
    collect_items[collect_count].hwnd = hwnd;
    collect_items[collect_count].pid = pid;
    collect_items[collect_count].id = collect_nextid++;
    lstrcpyn(collect_items[collect_count].exe, exe, ARR_SZ(collect_items[collect_count].exe));
    collect_items[collect_count].guid = CollectGuid(exe, hwnd);

    CollectFillIdentity(&nid, collect_count);
    nid.uFlags |= NIF_MESSAGE|NIF_ICON|NIF_TIP;
    nid.hIcon = GetWindowIcon(hwnd);
    CollectWindowText(hwnd, tip, ARR_SZ(tip));
    if (!tip[0]) lstrcpyn(tip, exe, ARR_SZ(tip));
    lstrcpyn(nid.szTip, tip, ARR_SZ(nid.szTip));

    for (i = 0; i < 3; i++) {
        if (Shell_NotifyIcon(NIM_ADD, (PNOTIFYICONDATA)&nid))
            break;
        Sleep(100);
    }
    if (i >= 3) {
        /* No icon: undo the hide so the window is not lost. */
        ShowWindow(hwnd, SW_SHOW);
        return;
    }
    collect_count++;
    CollectSaveConfig();
}

/* --- single instance redirect ---------------------------------------- */

/* Comma separated exe list, same matching rule as [Blacklist] Processes:
 * exact case insensitive exe name; a lone "*" is not usable here because we
 * only ever have one name to compare. */
static int collect_app_matches(const TCHAR *listbuf, const TCHAR *exe)
{
    const TCHAR *p = listbuf;
    if (!exe || !*exe || !p) return 0;
    while (*p) {
        TCHAR name[MAX_PATH];
        size_t n = 0;
        while (*p == TEXT(',') || *p == TEXT(' ')) p++;
        while (*p && *p != TEXT(',') && n < ARR_SZ(name)-1) name[n++] = *p++;
        name[n] = TEXT('\0');
        if (n == 1 && name[0] == TEXT('*')) return 1;  /* "*" = every application */
        if (n && !lstrcmpi(exe, name)) return 1;
    }
    return 0;
}

/* A window that looks like a real, freshly shown application window. */
static int is_fresh_app_window(HWND hwnd)
{
    LONG_PTR style, ex;
    DWORD cloaked = 0;

    if (!hwnd || hwnd == g_hwnd) return 0;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return 0;
    if (!IsWindowVisible(hwnd)) return 0;
    if (GetWindow(hwnd, GW_OWNER)) return 0;
    ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return 0;
    style = GetWindowLongPtr(hwnd, GWL_STYLE);
    if (!(style & WS_CAPTION)) return 0;
    if (OredredWinVer() >= 0x06020000
    &&  SUCCEEDED(DwmGetWindowAttributeL(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
    &&  cloaked) return 0;
    /* GetWindowTextLength uses the cached title for other processes: no
     * cross-process message, so it is safe on the WinEvent thread. */
    if (GetWindowTextLength(hwnd) <= 0) return 0;
    return 1;
}

static void CollectClosePending(void)
{
    size_t i;
    KillTimer(g_hwnd, COLLECT_REDIR_TIMER);
    for (i = 0; i < ARR_SZ(collect_pending_close); i++) {
        HWND hwnd = collect_pending_close[i];
        collect_pending_close[i] = NULL;
        if (hwnd && IsWindow(hwnd)) PostMessage(hwnd, WM_CLOSE, 0, 0);
    }
}

/* "Single instance" for the listed applications: launching the app again
 * gives back the collected window instead of a second one. */
static void CollectRedirectMaybe(HWND hwnd)
{
    TCHAR exe[MAX_PATH] = {0};
    DWORD pid = 0;
    int i;
    size_t k;

    if (!collect_enabled || !collect_processes[0]) return;
    if (collect_count == 0) return;   /* nothing in the tray: nothing to bring back */
    if (!is_fresh_app_window(hwnd)) return;
    if (!GetWindowProgName(hwnd, exe, ARR_SZ(exe))) return;
    if (!collect_app_matches(collect_processes, exe)) return;
    if (collect_app_matches(collect_multi, exe)) return;  /* user allowed multi */

    GetWindowThreadProcessId(hwnd, &pid);
    for (i = 0; i < collect_count; i++) {
        if (!lstrcmpi(collect_items[i].exe, exe)) break;
    }
    if (i >= collect_count) return;   /* this application is not in the tray */
    /* Same live process: another window of the collected application, not a
     * freshly started instance. Closing it would lose the user's work. */
    if (collect_items[i].pid == pid) return;

    CollectRestoreIndex(i);

    for (k = 0; k < ARR_SZ(collect_pending_close); k++) {
        if (!collect_pending_close[k]) break;
    }
    if (k >= ARR_SZ(collect_pending_close)) k = 0;   /* overwrite the oldest */
    collect_pending_close[k] = hwnd;
    /* A new window is often not ready at CREATE time: close it a bit later. */
    SetTimer(g_hwnd, COLLECT_REDIR_TIMER, 400, NULL);
}

static void CollectToggleTopmost(HWND hwnd)
{
    LONG_PTR ex;
    if (!IsWindow(hwnd)) return;
    ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    SetWindowPos(hwnd, (ex & WS_EX_TOPMOST) ? HWND_NOTOPMOST : HWND_TOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}

static void CollectCloseIndex(int i)
{
    HWND hwnd;
    if (i < 0 || i >= collect_count) return;
    hwnd = collect_items[i].hwnd;
    CollectRemoveIndex(i);
    if (IsWindow(hwnd)) PostMessage(hwnd, WM_CLOSE, 0, 0);
}

static void CollectRestoreAll(void)
{
    int i;
    for (i = 0; i < collect_count; i++)
        CollectRestoreIndex(i);
}

/* explorer.exe restarted: every Shell_NotifyIcon registration is gone. */
static void CollectReAddAll(void)
{
    int i;
    for (i = 0; i < collect_count; i++) {
        collect_nid_t nid;
        TCHAR tip[128];
        CollectFillIdentity(&nid, i);
        nid.uFlags |= NIF_MESSAGE|NIF_ICON|NIF_TIP;
        nid.hIcon = GetWindowIcon(collect_items[i].hwnd);
        CollectWindowText(collect_items[i].hwnd, tip, ARR_SZ(tip));
        if (!tip[0]) lstrcpyn(tip, TEXT(APP_NAMEA), ARR_SZ(tip));
        lstrcpyn(nid.szTip, tip, ARR_SZ(nid.szTip));
        Shell_NotifyIcon(NIM_ADD, (PNOTIFYICONDATA)&nid);
    }
}

/* Per-icon menu. TrackPopupMenu runs its own modal loop inside WindowProc,
 * which is the normal way to show a tray menu. */
static void CollectShowMenu(int i, POINT pt)
{
    HMENU menu = CreatePopupMenu();
    UINT topmost_flag = MF_STRING;
    int cmd;

    if (!menu) return;
    if (IsWindow(collect_items[i].hwnd)
    && (GetWindowLongPtr(collect_items[i].hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))
        topmost_flag |= MF_CHECKED;

    AppendMenu(menu, MF_STRING, 100 + i, l10n->InputActionRestore);
    AppendMenu(menu, topmost_flag, 300 + i, l10n->InputActionAlwaysOnTop);
    AppendMenu(menu, MF_STRING, 400 + i, l10n->InputActionClose);
    AppendMenu(menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(menu, MF_STRING, 200, l10n->CollectMenuRestoreAll);

    /* The classic two calls that make a tray menu dismiss itself. */
    SetForegroundWindow(g_hwnd);
    PostMessage(g_hwnd, WM_NULL, 0, 0);

    cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD|TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);

    if (cmd >= 400) {
        CollectCloseIndex(cmd - 400);
    } else if (cmd >= 300) {
        if (cmd - 300 < collect_count) CollectToggleTopmost(collect_items[cmd-300].hwnd);
    } else if (cmd >= 200) {
        CollectRestoreAll();
    } else if (cmd >= 100) {
        CollectRestoreIndex(cmd - 100);
    }
}

static void CollectIconMessage(WPARAM wParam, LPARAM lParam)
{
    UINT id = (UINT)wParam;
    int i;
    for (i = 0; i < collect_count; i++) {
        if (collect_items[i].id != id) continue;
        switch (LOWORD((DWORD_PTR)lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            CollectRestoreIndex(i);
            break;
        case WM_RBUTTONUP:
        {
            POINT pt;
            GetCursorPos(&pt);
            CollectShowMenu(i, pt);
            break;
        }
        }
        return;
    }
}

/* WINEVENT_OUTOFCONTEXT delivers this callback on the installing thread (the
 * main thread), so touching the table directly is safe. */
static void CALLBACK CollectWinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                         LONG idObject, LONG idChild,
                                         DWORD idEventThread, DWORD dwmsEventTime)
{
    int i;
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (event == EVENT_OBJECT_DESTROY) {
        i = CollectFindIndex(hwnd);
        if (i >= 0) CollectRemoveIndex(i);
        return;
    }
    if (event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_SHOW) return;

    /* A known window is never a fresh instance: the tray icon stays, the
     * entry only disappears when the window itself is destroyed. */
    i = CollectFindIndex(hwnd);
    if (i >= 0) return;
    CollectRedirectMaybe(hwnd);
}

static HWINEVENTHOOK collect_destroy_hook = NULL;

/* Persist the hidden windows so a crash or a restart can never leave windows
 * the user cannot reach. Format: "hwnd:pid,hwnd:pid". */
static void CollectSaveConfig(void)
{
    TCHAR buf[1024];
    sb_t sb = { buf, 0, ARR_SZ(buf) };
    int i;

    buf[0] = TEXT('\0');
    for (i = 0; i < collect_count; i++) {
        if (i) SbAppendStr(&sb, TEXT(","));
        SbAppendUint(&sb, (UINT)(UINT_PTR)collect_items[i].hwnd);
        SbAppendStr(&sb, TEXT(":"));
        SbAppendUint(&sb, (UINT)collect_items[i].pid);
    }
    WritePrivateProfileString(TEXT("TrayCollect"), TEXT("Windows"), buf, inipath);
}

static UINT CollectParseUint(const TCHAR **pp)
{
    UINT v = 0;
    const TCHAR *p = *pp;
    while (*p >= TEXT('0') && *p <= TEXT('9')) {
        v = v * 10 + (UINT)(*p - TEXT('0'));
        p++;
    }
    *pp = p;
    return v;
}

/* Startup: give every still-existing hidden window back to the user.
 * We deliberately do NOT rebuild the tray icons: if the recovery entry point
 * is lost once, the window can never be reached again. Safety over fidelity. */
static void CollectLoadWindows(void)
{
    TCHAR buf[1024];
    const TCHAR *p;

    buf[0] = TEXT('\0');
    GetPrivateProfileString(TEXT("TrayCollect"), TEXT("Windows"), TEXT(""), buf, ARR_SZ(buf), inipath);

    p = buf;
    while (*p) {
        HWND hwnd;
        UINT h, pid;
        DWORD pid2 = 0;
        while (*p == TEXT(',') || *p == TEXT(' ')) p++;
        if (!*p) break;
        h = CollectParseUint(&p);
        if (*p != TEXT(':')) break;
        p++;
        pid = CollectParseUint(&p);
        hwnd = (HWND)(UINT_PTR)h;
        if (hwnd && IsWindow(hwnd)
        && GetWindowThreadProcessId(hwnd, &pid2) && pid2 == pid
        && !IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_SHOW);
        }
        while (*p && *p != TEXT(',')) p++;
    }
    WritePrivateProfileString(TEXT("TrayCollect"), TEXT("Windows"), TEXT(""), inipath);
}

static void CollectReloadConfig(void)
{
    collect_enabled = GetPrivateProfileInt(TEXT("TrayCollect"), TEXT("Enabled"), 0, inipath) ? 1 : 0;
    collect_processes[0] = TEXT('\0');
    collect_multi[0] = TEXT('\0');
    GetPrivateProfileString(TEXT("TrayCollect"), TEXT("Processes"), TEXT(""),
                           collect_processes, ARR_SZ(collect_processes), inipath);
    GetPrivateProfileString(TEXT("TrayCollect"), TEXT("MultiInstance"), TEXT(""),
                           collect_multi, ARR_SZ(collect_multi), inipath);
}

/* Exit: give everything back, remove every icon, forget the session. */
static void CollectShutdown(void)
{
    while (collect_count > 0) {
        HWND hwnd;
        collect_count--;
        hwnd = collect_items[collect_count].hwnd;
        CollectDropIcon(collect_count);
        if (IsWindow(hwnd))
            ShowWindow(hwnd, SW_SHOW);
    }
    if (collect_destroy_hook) {
        UnhookWinEvent(collect_destroy_hook);
        collect_destroy_hook = NULL;
    }
    WritePrivateProfileString(TEXT("TrayCollect"), TEXT("Windows"), TEXT(""), inipath);
}

static void CollectInit(void)
{
    /* Must stay in sync with WindowProc's WM_TRAYCOLLECTMSG case: a dynamic
     * RegisterWindowMessage() id would never match that constant. */
    collect_cbmsg = WM_TRAYCOLLECTMSG;
    collect_count = 0;
    collect_nextid = 1;
    collect_destroy_hook =
        SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, NULL, CollectWinEventProc,
                        0, 0, WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    CollectReloadConfig();
    CollectLoadWindows();
}

#endif /* COLLECT_C_9F31 */
