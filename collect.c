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

typedef struct collect_item {
    HWND  hwnd;
    DWORD pid;
    UINT  id;
    GUID  guid;
} collect_item_t;

static collect_item_t collect_items[COLLECT_MAX];
static int  collect_count = 0;
static UINT collect_nextid = 1;
static UINT collect_cbmsg = 0;   /* Shell_NotifyIcon callback message */

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

static void CollectRestoreIndex(int i)
{
    HWND hwnd;
    if (i < 0 || i >= collect_count) return;
    hwnd = collect_items[i].hwnd;
    CollectDropIcon(i);
    collect_items[i] = collect_items[--collect_count];
    if (IsWindow(hwnd)) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    }
    CollectSaveConfig();
}

static void CollectWindow(HWND hwnd)
{
    collect_nid_t nid;
    TCHAR exe[MAX_PATH] = {0};
    TCHAR tip[128];
    DWORD pid = 0;
    int i;

    if (!hwnd || !IsWindow(hwnd)) return;
    if (collect_count >= COLLECT_MAX) return;      /* do not swallow the click */
    if (CollectFindIndex(hwnd) >= 0) return;       /* already collected */
    if (!GetWindowProgName(hwnd, exe, ARR_SZ(exe))) return;

    ShowWindow(hwnd, SW_HIDE);
    if (IsWindowVisible(hwnd)) {
        /* Could not hide it (elevated target / UIPI): honour the close
         * instead of leaving a window the user can neither see nor close. */
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        return;
    }

    GetWindowThreadProcessId(hwnd, &pid);
    collect_items[collect_count].hwnd = hwnd;
    collect_items[collect_count].pid = pid;
    collect_items[collect_count].id = collect_nextid++;
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
        }
        return;
    }
}

static void CollectSaveConfig(void)
{
}

static void CollectReloadConfig(void)
{
}

static void CollectInit(void)
{
    collect_cbmsg = RegisterWindowMessage(TEXT("AltSnap-TrayCollect"));
    collect_count = 0;
    collect_nextid = 1;
}

#endif /* COLLECT_C_9F31 */
