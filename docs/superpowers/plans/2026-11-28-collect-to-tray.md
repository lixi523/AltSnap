# 关闭即收入托盘 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 AltSnap 把指定应用的窗口「关闭即收进系统托盘」，每个窗口一个可被系统独立排列的托盘图标，并支持每应用可切换的「唯一实例」。

**Architecture:** 检测在 `hooks.dll`（`LowLevelMouseProc` 判 `HTCLOSE`、`LowLevelKeyboardProc` 判 Alt+F4，命中就吞掉输入并向主窗口发 `WM_TRAYCOLLECT`），策略与状态在主程序侧的新模块 `collect.c`（隐藏窗口、`Shell_NotifyIcon` 逐图标注册、还原、持久化、唯一实例重定向）。`hooks.dll` 依旧只在 AltSnap 进程内运行，不注入。

**Tech Stack:** 纯 C99 + Win32，`-m32` gcc（`-nostdlib`，`-DUNICODE -D_UNICODE -fshort-wchar`），构建工具链是仓库内自带的 `tools/w64devkit`。

**Spec:** `docs/superpowers/specs/2026-11-28-collect-to-tray-design.md`

## Global Constraints

- 构建命令固定为（每个任务的验收都用它，且原文照抄）：
  ```powershell
  $env:PATH = "D:\Documents\Code\AltSnap\tools\w64devkit\bin;$env:PATH"
  cd D:\Documents\Code\AltSnap
  Remove-Item hooks.dll,hooksr.o -ErrorAction SilentlyContinue   # 只在切换架构后第一次需要
  make -fMakefileX64 -j2
  ```
  期望：`exit=0`，且**不新增任何 warning**。基线里已知的两条 warning（`nanolibc.h:367 -Wchar-subscripts`、`hooks.c` `Unload()` 里的 `-Wpointer-to-int-cast`）是既有问题，不属于本任务。
- **用 `MakefileX64`，不要用 `make -j2`**：工作区里既有的 `altsnapr.o` 是 x86-64 COFF，与 32 位 `Makefile` 链接会报 `incompatible with i386 output`。切换架构后必须先删 `hooks.dll`/`hooksr.o` 再编，否则 make 会复用旧的 32 位 `hooks.dll`，导致 64 位主程序加载失败。
- 不要用 `mk.bat` / `mk64.bat`（它们会 `taskkill AltSnap.exe` 再 `start AltSnap.exe`，干扰手工验证）。
- 构建产物（`AltSnap.exe`、`hooks.dll`、`altsnapr.o`、`hooksr.o`）**已被 .gitignore 忽略**，不需要也不应该 `git checkout` 它们；提交只涉及源码。
- 字符串一律 `TCHAR` / `lstr*` / `wsprintf` / `SbAppendStr`，不引入 CRT（`-nostdlib`）。不要用 `snprintf`、`strcpy`、`wchar_t` 字面量。
- ini 段名与键名固定：`[TrayCollect]`，键 `Enabled` / `Processes` / `MultiInstance` / `Windows`。逗号分隔，逗号后空格跳过（与 `[Blacklist] Processes` 同一套语义）。
- 消息号固定：`WM_TRAYCOLLECT = WM_USER+21`（hooks.dll → 主程序），`WM_TRAYCOLLECTMSG = WM_USER+22`（`Shell_NotifyIcon` 回调）。
- 资源 ID 固定：`IDD_COLLECTPAGE = 209`，`IDC_COLLECT_BOX 2300`、`IDC_COLLECT_ENABLED 2301`、`IDC_COLLECT_LIST 2302`、`IDC_COLLECT_ADD 2303`、`IDC_COLLECT_FIND 2304`、`IDC_COLLECT_HINT 2305`、`IDC_COLLECT_EDIT 2306`、`IDC_COLLECT_DEL 2307`。
- 上限 `COLLECT_MAX = 32`；满了就放弃收纳（**不吞点击**，窗口正常关闭）。
- **本项目没有测试框架，且改动集中在 Win32 钩子 / 托盘 / GUI**，无法用单元测试固定行为。每个任务的验收 = 【`make` 零警告】+【下列手工步骤 + 可观察结果】。这是刻意选择：为这些行为搭一套 C 测试桩，成本远高于收益，且测不到真实输入链路。
- 需要日志时用 `make -j2 EXTRA_CFLAGS=-DLOG_STUFF`，运行时日志写到 exe 旁 `ad.log`（`unfuck.h:299`）。默认构建里 `LOG` 是空操作。

## Review Focus

以下 5 个输入/失败模式是 spec 隐含会出现、但没有任何任务的测试能自动覆盖的，最可能伤到真实用户；每条都在对应任务的验收步骤里有手工用例。

1. **目标窗口在收纳瞬间正在销毁**（点 X 后应用立刻退出）→ 不得留下幽灵托盘图标或悬空表项（Task 1 Step 5、Task 2 Step 4）。
2. **被收纳窗口的进程崩溃或被任务管理器结束** → 图标必须消失，`Windows=` 里的死条目在下次启动被忽略（Task 2 Step 4、Task 3 Step 4）。
3. **同一窗口快速连点关闭按钮** → 不得产生两个图标或重复隐藏（Task 1 Step 5）。
4. **托盘图标数达到 32 上限** → 收纳被放弃且窗口正常关闭，用户仍能关掉窗口（Task 1 Step 5）。
5. **ini 被写坏**（`Processes` 只有逗号、空项、超长、含中文路径）→ 不崩溃、不误匹配、不吞错窗口的点击（Task 4 Step 6）。

## File Structure

| 文件 | 责任 |
| --- | --- |
| `collect.c`（新建，仓库根目录） | 主程序侧全部托盘收纳逻辑：配置读写、隐藏/还原、图标、右键菜单、销毁清理、持久化、唯一实例重定向。被 `altsnap.c` 直接 `#include`，不单独编译。 |
| `hooks.c`（改） | 只做检测与通知：`CollectList` 装载、`HTCLOSE` 拦截、Alt+F4 拦截。 |
| `hooks.h`（改） | 两个消息号。 |
| `altsnap.c`（改） | `#include "collect.c"`、`WindowProc` 分支、`CollectInit()`、`WM_TASKBARCREATED` / `WM_DESTROY` 挂接。 |
| `config.c`（改） | 设置页：`pages[]`、`titles[]`、`CollectPageDialogProc`、`FindWindowProc` 增加一段。 |
| `window.rc` / `resource.h`（改） | 对话框模板与控件 ID。 |
| `languages.h` / `Lang/zh_CN.ini`（改） | 翻译键与中文。 |
| `AltSnap.dni`（改） | `[TrayCollect]` 段与注释。 |
| `Makefile` 等 9 个 `Makefile*`（改） | `AltSnap.exe` 依赖加 `collect.c`。 |

---

### Task 1: 端到端最小闭环（关闭 → 隐藏 → 单图标 → 单击还原）

**Files:**
- Create: `collect.c`
- Modify: `hooks.h:99-100`（`WM_GETZONES` 之后加两个消息号）
- Modify: `hooks.c:6413`（`readallblacklists` 之后加 `readcollectlist`）、`hooks.c:6787`（`Load()` 里调用）、`hooks.c:5477`（`LowLevelMouseProc` 里 `enum button button = ...` 之后）
- Modify: `altsnap.c:41`（`#include "tray.c"` 之前）、`altsnap.c:449`（`WindowProc` 消息链）、`altsnap.c:512`（`InitTray()` 之后）
- Modify: 9 个 `Makefile*` 的 `AltSnap.exe :` 依赖行，追加 `collect.c`

**Interfaces:**
- Consumes（都已在仓库里）：
  - `GetWindowProgName(HWND hwnd, TCHAR *title, size_t len) -> DWORD`（`unfuck.h:1361`，返回 PID 并把 exe **basename** 写进 `title`，已处理 `ApplicationFrameHost` 穿透）
  - `blacklisted_from_names(const blacklist_t*, const TCHAR *exename, const TCHAR *title, const TCHAR *classname) -> int`（`hooks.c:469`）
  - `HitTestTimeoutL(HWND, LPARAM) -> int`（`unfuck.h:1564`，`SendMessageTimeout` 255ms）
  - `lstrcmp_star(a, b)`（`nanolibc.h:420`，`b == NULL` 视为匹配）
  - `readblacklist(const TCHAR *section, blacklist_t *list, const char *key)`（`hooks.c:6329`）
- Produces（后续任务依赖这些名字与签名）：
  - `#define COLLECT_MAX 32`
  - `typedef struct { HWND hwnd; DWORD pid; UINT id; GUID guid; } collect_item_t;`
  - `static collect_item_t collect_items[COLLECT_MAX]; static int collect_count; static UINT collect_nextid;`
  - `void CollectInit(void);`
  - `void CollectWindow(HWND hwnd);`
  - `void CollectIconMessage(WPARAM wParam, LPARAM lParam);`
  - `int  CollectFindIndex(HWND hwnd);`（找不到返回 -1）
  - `void CollectRestoreIndex(int i);`
  - `void CollectRemoveIndex(int i);`（只撤图标并移出表，不动窗口）
  - `void CollectSaveConfig(void);`（Task 3 实现，Task 1 先留空函数体）
  - `void CollectReloadConfig(void);`（Task 4 实现，Task 1 先留空函数体）
  - `UINT collect_cbmsg;`（`RegisterWindowMessage(TEXT("AltSnap-TrayCollect"))`）

- [ ] **Step 0: 记录基线**

手工开一个记事本，点 X，确认它**正常关闭**（这是改动前的行为，也是回归基准）。

- [ ] **Step 1: 加两个消息号**

`hooks.h`，在 `#define WM_GETZONES       (WM_USER+20)` 下一行加：

```c
#define WM_TRAYCOLLECT    (WM_USER+21)   /* hooks.dll -> main: please collect this hwnd */
#define WM_TRAYCOLLECTMSG (WM_USER+22)   /* Shell_NotifyIcon callback */
```

- [ ] **Step 2: 写 `collect.c` 的数据与收纳/还原**

在仓库根目录新建 `collect.c`，按下面给出的接口写实现。不需要 `#include` 任何东西（`altsnap.c` 已包含全部头文件）。

必须有：

```c
#define COLLECT_MAX 32
typedef struct { HWND hwnd; DWORD pid; UINT id; GUID guid; } collect_item_t;
static collect_item_t collect_items[COLLECT_MAX];
static int collect_count;
static UINT collect_nextid = 1;
static UINT collect_cbmsg;

/* 自带一份 V3 通知结构：A/W 构建都要能编，且要带 guidItem */
typedef struct {
    DWORD cbSize; HWND hWnd; UINT uID; UINT uFlags; UINT uCallbackMessage;
    HICON hIcon; TCHAR szTip[128]; DWORD dwState, dwStateMask;
    TCHAR szInfo[256]; UINT uVersion; TCHAR szInfoTitle[64];
    DWORD dwInfoFlags; GUID guidItem; HICON hBalloonIcon;
} collect_nid_t;
```

`CollectGuid(const TCHAR *exe, HWND hwnd) -> GUID`：FNV-1a 64 位（offset basis `0xCBF29CE484222325`，prime `0x100000001B3`）对 `"AltSnap/Collect/" + exe + "/" + <hwnd 十进制>` 求哈希，把 8 字节结果写进 `GUID` 的前 8 字节，再令 `Data3` 高位为 `0x40xx`（版本 4）、`Data4[0]` 高两位为 `10`（变体）。**每个窗口的 GUID 必须不同**，否则 Windows 会把它们当成同一个图标。

`CollectWindow(HWND hwnd)` 顺序固定：
1. `if (collect_count >= COLLECT_MAX) return;`（不吞点击，窗口照常关闭 —— 见 Review Focus #4）
2. 已在表里 → 直接返回（Review Focus #3）。
3. exe 名：`TCHAR exe[MAX_PATH] = {0}; GetWindowProgName(hwnd, exe, ARR_SZ(exe)); if (!exe[0]) return;`
4. 图标：`WM_GETICON(ICON_BIG)` → `WM_GETICON(ICON_SMALL2)` → `GetClassLongPtr(GCLP_HICON)` → `GetClassLongPtr(GCLP_HICONSM)` → `LoadIcon(g_hinst, MAKEINTRESOURCE(APP_ICON))`。
5. `ShowWindow(hwnd, SW_HIDE); if (IsWindowVisible(hwnd)) { PostMessage(hwnd, WM_CLOSE, 0, 0); return; }`
6. 填 `collect_nid_t`：`cbSize = sizeof(collect_nid_t)`、`hWnd = g_hwnd`、`uID = collect_nextid++`、`uFlags = NIF_MESSAGE|NIF_ICON|NIF_TIP`、`uCallbackMessage = collect_cbmsg`、`hIcon`、`guidItem = CollectGuid(exe, hwnd)`；`szTip` 用 `lstrcpyn` 截到 127。
7. `Shell_NotifyIcon(NIM_ADD, ...)` 失败重试 3 次、间隔 `Sleep(100)`；仍失败则 `ShowWindow(hwnd, SW_SHOW)` 回滚并 `return`。
8. 写表和 `collect_count++`，然后 `CollectSaveConfig()`（本任务为空函数）。

`CollectRestoreIndex(int i)`：`ShowWindow(hwnd, SW_SHOW)`；`SetForegroundWindowL(hwnd)`（`unfuck.h` 已有，别用裸 `SetForegroundWindow`）；`Shell_NotifyIcon(NIM_DELETE, ...)`；从表里移除（后面的元素前移），`collect_count--`，`CollectSaveConfig()`。

`CollectIconMessage`：`switch (LOWORD(lParam))` 的 `WM_LBUTTONUP` 与 `WM_LBUTTONDBLCLK` → 用 `wParam`（`uID`）找到下标后 `CollectRestoreIndex`。右键菜单留到 Task 2。

`CollectInit(void)`：`collect_cbmsg = RegisterWindowMessage(TEXT("AltSnap-TrayCollect"));` 以及 `collect_count = 0; collect_nextid = 1;`。

- [ ] **Step 3: 让 hooks.dll 报告关闭动作**

`hooks.c`，`static const char *BlackListStrings[]` 附近加：

```c
static blacklist_t CollectList;
```

`readallblacklists()`（`hooks.c:6413`）之后加：

```c
static void readcollectlist(const TCHAR *inipath)
{
    enum { len = 32767 };
    mem00(&CollectList, sizeof(CollectList));
    TCHAR *section = (TCHAR *)malloc(len*sizeof(TCHAR));
    if (!section) return;
    GetPrivateProfileSection(TEXT("TrayCollect"), section, len, inipath);
    if (GetPrivateProfileInt(TEXT("TrayCollect"), TEXT("Enabled"), 0, inipath))
        readblacklist(section, &CollectList, "Processes");
    free(section);
}
```

在 `Load()`（`hooks.c:6652`）里 `readallblacklists(inipath);`（`hooks.c:6787`）的下一行调用 `readcollectlist(inipath);`。

`LowLevelMouseProc`（`hooks.c:5437`）里，`enum button button = (enum button)GetButton(wParam, lParam);` 之后、`action_t action = GetAction(button);` 之前插入：

```c
if (CollectList.length && (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP)) {
    if (collect_close_click(pt, wParam == WM_LBUTTONUP))
        return 1;
}
```

`collect_close_click(POINT pt, int is_up)` 放在 `LowLevelMouseProc` 之前，顺序**不得调换**：

```c
static int collect_close_click(POINT pt, int is_up)
{
    static HWND cached_hwnd;
    static int  cached_hit;
    static HWND pending_hwnd;
    HWND hwnd = WindowFromPoint(pt);
    if (!hwnd) return 0;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root || root == g_mainhwnd) return 0;
    if (root != cached_hwnd) {
        TCHAR exe[MAX_PATH] = {0};
        cached_hwnd = root;
        cached_hit = 0;
        if (GetWindowProgName(root, exe, ARR_SZ(exe)))
            cached_hit = blacklisted_from_names(&CollectList, exe, NULL, NULL);
    }
    if (!cached_hit) return 0;
    if (HitTestTimeoutL(hwnd, MAKELPARAM(pt.x, pt.y)) != HTCLOSE) return 0;
    if (is_up) {
        if (root != pending_hwnd) return 0;
        pending_hwnd = NULL;
        return 1;
    }
    pending_hwnd = root;
    PostMessage(g_mainhwnd, WM_TRAYCOLLECT, (WPARAM)root, 0);
    return 1;
}
```

**禁止**在这条路径里用 `GetWindowText` / `HitTestTimeoutbl` / `blacklisted`（它们内部会发无超时的跨进程同步消息，钩子里会冻结输入流）。

Alt+F4 拦截放在 `LowLevelKeyboardProc`（`hooks.c:2744`）里 `if (ScrollLockState()) return CallNextHookEx(...);`（`hooks.c:2761`）之后、`if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)`（`hooks.c:2763`）之前：

```c
if (CollectList.length && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN || wParam == WM_SYSKEYUP)
&& vkey == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) {
    TCHAR exe[MAX_PATH] = {0};
    HWND root = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (root && GetWindowProgName(root, exe, ARR_SZ(exe))
    &&  blacklisted_from_names(&CollectList, exe, NULL, NULL)) {
        if (wParam != WM_SYSKEYUP)
            PostMessage(g_mainhwnd, WM_TRAYCOLLECT, (WPARAM)root, 0);
        return 1;
    }
}
```

- [ ] **Step 4: 主程序接线**

`altsnap.c`：`#include "tray.c"`（第 41 行）改为

```c
#include "tray.c"
#include "collect.c"
```

`WindowProc`（`altsnap.c:449`）消息链里加两个分支（放在 `msg == WM_TRAY` 分支之前）：

```c
} else if (msg == WM_TRAYCOLLECT) {
    CollectWindow((HWND)wParam);
} else if (msg == WM_TRAYCOLLECTMSG) {
    CollectIconMessage(wParam, lParam);
```

`tWinMain` 里 `InitTray();`（`altsnap.c:512`）之后加 `CollectInit();`。

9 个 `Makefile*` 的 `AltSnap.exe :` 依赖行末尾追加 ` collect.c`。

- [ ] **Step 5: 构建并手工验证**

运行 Global Constraints 里的 `make -j2`，期望零 error、零新 warning。

准备 ini（`%APPDATA%\AltSnap\AltSnap.ini`，或 exe 同目录的 `AltSnap.ini`；改完需重启 AltSnap）：

```ini
[TrayCollect]
Enabled=1
Processes=notepad.exe
```

逐条验证：
1. 开记事本 → 点 X → 窗口消失，托盘多出一个记事本图标（不是 AltSnap 主图标）。
2. 单击该图标 → 记事本回来并到前台。
3. 再点 X 两次（快速连点）→ 只出现**一个**图标，表里只有一条（图标数不会变成 2）。
4. 开 3 个记事本、各自点 X → 3 个独立图标；在 Win10 上拖动其中一个，其余不跟随。
5. 把记事本加入列表后，用 `mspaint.exe`（未列入）点 X → 正常关闭。
6. 临时把 `Processes` 写到 32 个不同 exe 并填满图标后再点 X → 窗口正常关闭（不吞点击）。

- [ ] **Step 6: 提交**

```bash
git add collect.c hooks.c hooks.h altsnap.c Makefile MakefileARM64Docker MakefileClang MakefileClangdb Makefiledb MakefileGCC3 MakefileTCC MakefileX64 MakefileX64db
git commit -m "feat: 关闭按钮/Alt+F4 收入托盘（单图标 + 单击还原）"
```

---

### Task 2: 图标右键菜单、窗口销毁清理、explorer 重启重建

**Files:**
- Modify: `collect.c`（新增 `CollectMenuCommand`、`CollectRemoveByHwnd`、`CollectReAddAll`、`CollectWinEventProc`）
- Modify: `altsnap.c`（`WM_TASKBARCREATED` 分支、`CollectInit` 里装 WinEvent 钩子、`WindowProc` 加菜单命令分支）

**Interfaces:**
- Consumes: Task 1 的 `collect_items` / `collect_count` / `collect_cbmsg` / `CollectRestoreIndex` / `CollectRemoveIndex`
- Produces:
  - `void CollectReAddAll(void);`
  - `void CollectWinEventProc(HWINEVENTHOOK h, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD tid, DWORD time);`（`CALLBACK`）
  - `void CollectCloseWindow(int i);`（`PostMessage(hwnd, WM_CLOSE)` 后 `CollectRemoveIndex(i)`）
  - 菜单命令号从 `100` 起：`100 + i` = 还原第 i 项，`200` = 全部还原，`300 + i` = 置顶切换第 i 项，`400 + i` = 关闭第 i 项

- [ ] **Step 1: 右键菜单**

`collect_cbmsg` 的 `WM_RBUTTONUP` → `SetForegroundWindow(g_hwnd); PostMessage(g_hwnd, WM_NULL, 0, 0);` 然后 `TrackPopupMenu`（照 `tray.c:ShowContextMenu` 的写法），菜单项按 spec §7.3：还原 / 置顶 / 关闭 / 分隔 / 全部还原。`TrackPopupMenu` 用 `TPM_RETURNCMD`，用返回值直接派发，不走 `WM_COMMAND`。

置顶：`GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST` 取反后 `SetWindowPos(hwnd, HWND_TOPMOST/HWND_NOTOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE)`。

- [ ] **Step 2: 窗口销毁清理**

`CollectWinEventProc` 里 `if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;`，然后 `if (event == EVENT_OBJECT_DESTROY) CollectRemoveByHwnd(hwnd);`。`CollectRemoveByHwnd` = `CollectFindIndex` 后 `CollectRemoveIndex`。

`CollectInit` 里装钩子：

```c
SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, NULL, CollectWinEventProc,
                0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
```

（`WINEVENT_OUTOFCONTEXT` 的回调在安装它的线程、即主线程派发，所以可以直接改表。）

- [ ] **Step 3: explorer 重启重建**

`altsnap.c` 的 `msg == WM_TASKBARCREATED` 分支里，`tray_added = 0; UpdateTray();` 之后加 `CollectReAddAll();`。`CollectReAddAll` 对表里每项重新 `NIM_ADD`（同一个 `uID` / `guidItem`）。

- [ ] **Step 4: 构建并手工验证**

`make -j2`，零 error、零新 warning。然后：
1. 收纳两个记事本 → 右键其中一个图标 → 「置顶」→ 该窗口无边框置顶标记；再点一次取消。
2. 右键 → 「关闭」→ 该窗口真关闭、图标消失、另一个不受影响。
3. 右键 → 「全部还原」→ 两个窗口都回来、所有图标消失。
4. 收纳一个记事本 → 在任务管理器里结束该记事本进程 → 图标**自动消失**（Review Focus #2）。
5. 收纳一个记事本 → 任务管理器 → 重启 `explorer.exe` → 图标自动回来。
6. 收纳后立刻点 X（应用瞬间退出，如某些启动器）→ 不留幽灵图标（Review Focus #1）。

- [ ] **Step 5: 提交**

```bash
git add collect.c altsnap.c
git commit -m "feat: 收纳图标的右键菜单、销毁清理与 explorer 重启重建"
```

---

### Task 3: 持久化与「隐形窗口」防线

**Files:**
- Modify: `collect.c`（实现 `CollectSaveConfig`、`CollectLoadWindows`；`CollectShutdown`）
- Modify: `altsnap.c`（`WM_DESTROY` 分支）

**Interfaces:**
- Consumes: `inipath`（`altsnap.c` 全局）、`CollectRestoreIndex`、`collect_items`
- Produces:
  - `void CollectSaveConfig(void);`（实现 Task 1 留着的空函数）
  - `void CollectLoadWindows(void);`
  - `void CollectShutdown(void);`

- [ ] **Step 1: 写 `Windows=`**

`CollectSaveConfig()` 把表里每项拼成 `hwnd 十进制:pid 十进制`，逗号分隔（空表写空串），用 `WritePrivateProfileString(TEXT("TrayCollect"), TEXT("Windows"), buf, inipath)` 写回。缓冲用栈上 `TCHAR[1024]` + `SbAppendStr`/`sb_t`（`unfuck.h`，`tray.c:catFullLayoutName` 有用法示例）。

在 `CollectWindow` 成功、`CollectRestoreIndex`、`CollectRemoveIndex` 三处末尾调用它。

- [ ] **Step 2: 启动时还原遗留窗口**

`CollectLoadWindows()`：`GetPrivateProfileString` 取 `Windows`，按 `,` 切分，每项解析 `hwnd:pid`；仅当 `IsWindow(hwnd)` 为真、`GetWindowThreadProcessId(hwnd,&pid2)` 与记录的 pid 相等、且 `!IsWindowVisible(hwnd)` 时，`ShowWindow(hwnd, SW_SHOW)`。处理完无条件把 `Windows` 写成空串。

在 `CollectInit()` 末尾调用它（`collect_count` 仍为 0：启动时只还原，不重建图标 —— 见 spec §7.5 的决定）。

- [ ] **Step 3: 退出时还原**

`CollectShutdown()`：遍历表，先全部 `NIM_DELETE`，再全部 `ShowWindow(SW_SHOW)`，清表，把 `Windows` 写成空串。`altsnap.c` 的 `WM_DESTROY` 分支里 `RemoveTray();` 之后加 `CollectShutdown();`。

- [ ] **Step 4: 构建并手工验证**

`make -j2`，零 error、零新 warning。然后：
1. 收纳两个记事本 → 正常退出 AltSnap → 两个窗口都回来，托盘无残留图标。
2. 收纳两个记事本 → 任务管理器结束 AltSnap 进程 → ini 里 `Windows=` 有两条 → 重新启动 AltSnap → 两个窗口**自动还原**，`Windows=` 被清空（Review Focus #2 的崩溃分支）。
3. 手工把 `Windows=0:0` 这种死条目写进 ini → 启动 AltSnap 不崩溃、不还原任何窗口、该键被清空。

- [ ] **Step 5: 提交**

```bash
git add collect.c altsnap.c
git commit -m "feat: 收纳状态的持久化与启动/退出还原"
```

---

### Task 4: 设置页（每个应用一个「唯一实例」勾选框）

**Files:**
- Modify: `resource.h`（追加 `IDD_COLLECTPAGE` 与 8 个 `IDC_COLLECT_*`）
- Modify: `window.rc`（在 `IDD_AWAKEPAGE` 之后追加 `IDD_COLLECTPAGE`）
- Modify: `config.c:23-26`（前置声明）、`config.c:125-134`（`pages[]`）、`config.c:180-193`（`titles[]`）、`config.c:1758` 附近（`FindWindowProc`）、新增 `CollectPageDialogProc`
- Modify: `collect.c`（实现 `CollectReloadConfig`）

**Interfaces:**
- Consumes: `ReadDialogOptions` / `WriteDialogOptions` / `optlst_t` / `V`（`config.c:470-500`、`config.c:537`）、`L10NIDX` / `dialogstring_t` / `UpdateDialogStrings`、`PropSheet_Changed`
- Produces:
  - `static void CollectReloadConfig(void);`（读 `Enabled` 到 `static int collect_enabled;`，读 `Processes` / `MultiInstance` 到两个逗号串缓冲 `static TCHAR collect_processes[2048]; static TCHAR collect_multi[2048];`）
  - `static INT_PTR CALLBACK CollectPageDialogProc(HWND, UINT, WPARAM, LPARAM);`

- [ ] **Step 1: 资源**

`resource.h` 末尾加：

```c
#define IDD_COLLECTPAGE                 209
#define IDC_COLLECT_BOX                 2300
#define IDC_COLLECT_ENABLED             2301
#define IDC_COLLECT_LIST                2302
#define IDC_COLLECT_ADD                 2303
#define IDC_COLLECT_FIND                2304
#define IDC_COLLECT_HINT                2305
#define IDC_COLLECT_EDIT                2306
#define IDC_COLLECT_DEL                 2307
```

`window.rc` 里 `IDD_AWAKEPAGE` 块（`window.rc:259-285`）之后加：

```rc
IDD_COLLECTPAGE DIALOG DISCARDABLE  0, 0, 220, 190
STYLE WS_CHILD | WS_VISIBLE | WS_CAPTION | WS_SYSMENU | DS_3DLOOK
CAPTION "" // TRAY COLLECT
FONT 8, "MS Shell Dlg 2"
BEGIN
    GROUPBOX        "",IDC_COLLECT_BOX, 3,1,212,186
    AUTOCHECKBOX    "",IDC_COLLECT_ENABLED, 10,10,198,8
    LTEXT           "",IDC_COLLECT_LIST, 10,20,200,8
    CONTROL         "",IDC_COLLECT_LIST,"SysListView32",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP|WS_BORDER, 10,30,200,80
    ICON            IDI_FIND,IDC_COLLECT_FIND, 10,116,16,16, SS_NOTIFY|SS_REALSIZEIMAGE
    EDITTEXT        IDC_COLLECT_EDIT, 30,118,96,12, ES_AUTOHSCROLL
    PUSHBUTTON      "",IDC_COLLECT_ADD, 130,117,34,14
    PUSHBUTTON      "",IDC_COLLECT_DEL, 168,117,34,14
    LTEXT           "",IDC_COLLECT_HINT, 10,136,200,44
END
```

注意 `LTEXT` 与 `SysListView32` **不能共用** `IDC_COLLECT_LIST`：列表标题改用 `IDC_STATIC`（`-1`）。上面第 3 行必须是 `LTEXT "", IDC_STATIC, 10,20,200,8`。

- [ ] **Step 2: 注册页面**

`config.c`：在 `AwakePageDialogProc` 的声明旁加 `static INT_PTR CALLBACK CollectPageDialogProc(HWND, UINT, WPARAM, LPARAM);`；`pages[]` 里在 `IDD_AWAKEPAGE` 之前插 `{ IDD_COLLECTPAGE, (DLGPROC)CollectPageDialogProc },`；`UpdateStrings()` 的 `const TCHAR *titles[7]` 改成 `titles[8]`，在 `titles[5]`（Awake）之后插 `titles[6] = l10n->ConfigTabCollect;`，并把原来的 About 挪到 `titles[7]`。`ARR_SZ(titles)` 不用改。

- [ ] **Step 3: 页面逻辑**

`CollectPageDialogProc`：

- `optlst` 只有一条：`{ IDC_COLLECT_ENABLED, T_BOL, 0, TEXT("TrayCollect"), "Enabled", V 0 },`，`WM_INITDIALOG` 里 `ReadDialogOptions`，`PSN_APPLY` 里 `WriteDialogOptions`。
- `WM_INITDIALOG`：`ListView_SetExtendedListViewStyle(GetDlgItem(hwnd,IDC_COLLECT_LIST), LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT);` 然后 `LVCOLUMN` 插一列（`mask = LVCF_TEXT|LVCF_WIDTH`、`pszText = TEXT("")`、`cx = 190`），再调 `CollectPageFillList(hwnd)`。
- `CollectPageFillList(HWND)`：`ListView_DeleteAllItems`，读 `GetPrivateProfileString(TEXT("TrayCollect"), TEXT("Processes"), TEXT(""), buf, ARR_SZ(buf), inipath)`，按 `,` 切分（`,` 前导空格跳过），每项 `ListView_InsertItem` + 默认勾选；再读 `MultiInstance`，对命中的项 `ListView_SetCheckState(i, FALSE)`。去重用 `lstrcmpi` 遍历已有行。
- `ListView_SetCheckState` 需要 `ListView_SetItemState(item, INDEXTOSTATEIMAGEMASK(checked?2:1), LVIS_STATEIMAGEMASK)`；`ListView_GetCheckState` 用 `SendMessage` 取 `LVIS_STATEIMAGEMASK` 后右移 12 位再减 1。
- `WM_COMMAND`：`IDC_COLLECT_ADD` → 取 `IDC_COLLECT_EDIT` 文本，非空且不重复则插入并勾选并 `PropSheet_Changed(g_cfgwnd, hwnd)`；`IDC_COLLECT_DEL` → 删选中行 + `PropSheet_Changed`。
- `WM_NOTIFY` → `LVN_ITEMCHANGED` 且 `(uChanged & LVIF_STATE)` → `PropSheet_Changed(g_cfgwnd, hwnd);`。**这一条不能省**：ListView 不产生 `EN_UPDATE`，漏了它用户只勾复选框时 Apply 按钮不会亮。
- `PSN_SETACTIVE`：`UpdateDialogStrings` 更新 6 个控件文案（`IDC_COLLECT_BOX`、`IDC_COLLECT_ENABLED`、`IDC_COLLECT_LIST`、`IDC_COLLECT_ADD`、`IDC_COLLECT_DEL`、`IDC_COLLECT_HINT`）。
- `PSN_APPLY`：遍历行拼两个逗号串 —— 命名的进 `Processes`，未勾选的再进 `MultiInstance`；`WritePrivateProfileString` 各写一次；然后 `CollectReloadConfig();` 与 `UpdateSettings();`。

- [ ] **Step 4: 准星填入 exe 名**

`FindWindowProc`（`config.c:1758`）里，在 `SetDlgItemText(page, IDC_NEWPROGNAME, txt_buf);` 那段附近加：若 `GetDlgItem(page, IDC_COLLECT_EDIT)` 存在，则把同一个 `txt_buf`（`GetWindowProgName` 的结果）设给它。黑名单页原有字段逻辑不动。

- [ ] **Step 5: `CollectReloadConfig`**

`collect.c` 里实现 Task 1 留的空函数：读 `Enabled` 到 `collect_enabled`，读 `Processes` / `MultiInstance` 到两个静态缓冲，供 Task 5 匹配使用。

- [ ] **Step 6: 构建并手工验证**

`make -j2`，零 error、零新 warning。然后：
1. 打开设置 → 新页签存在，位置在 Awake 与 About 之间。
2. 点准星再点一个记事本窗口 → `IDC_COLLECT_EDIT` 里填入 `notepad.exe`；点「添加」→ 列表出现一行且默认**已勾选**。
3. 只取消这一行的勾选（不改文字）→ Apply 按钮**变亮**（Review Focus 相关，`LVN_ITEMCHANGED` 生效）；点 Apply → ini 里 `Processes=notepad.exe`、`MultiInstance=notepad.exe`。
4. 重开设置页 → 那一行仍是未勾选。
5. 加第二项时不重复；点「删除」能删掉选中行；Apply 后 ini 同步。
6. 手工把 ini 写成 `Processes=,,notepad.exe,`（空项 + 多余逗号）→ 打开设置页不崩溃，只显示一项；点 X 时只有记事本被收纳，其它窗口不受影响（Review Focus #5）。

- [ ] **Step 7: 提交**

```bash
git add resource.h window.rc config.c collect.c
git commit -m "feat: 设置页新增托盘收纳页（应用列表 + 唯一实例勾选）"
```

---

### Task 5: 唯一实例重定向

**Files:**
- Modify: `collect.c`（`CollectRedirectMaybe`、`is_fresh_app_window`、`CollectClosePending`、pending 队列）
- Modify: `altsnap.c`（`WindowProc` 加 `WM_TIMER` 分支；`CollectInit` 里加 `SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, ...)`）
- Modify: `collect_item_t` 增加 `TCHAR exe[MAX_PATH];`

**Interfaces:**
- Consumes: Task 4 的 `collect_processes` / `collect_multi` 缓冲、`lstrcmp_star` / `lstrcmpi`、`DwmGetWindowAttributeL`（`unfuck.h`）
- Produces:
  - `static void CollectRedirectMaybe(HWND hwnd);`
  - `static int is_fresh_app_window(HWND hwnd);`
  - `#define COLLECT_REDIR_TIMER 0xC011`
  - `static HWND collect_pending_close[4];`

- [ ] **Step 1: 给表项补 exe 名**

`collect_item_t` 加 `TCHAR exe[MAX_PATH];`，`CollectWindow` 里把 `GetWindowProgName` 的结果 `lstrcpyn` 进去。`CollectGuid` 的输入改用这个字段。

- [ ] **Step 2: 匹配单个应用的辅助函数**

```c
static int collect_app_matches(const TCHAR *listbuf, const TCHAR *exe)  /* listbuf 为逗号串 */
static int collect_is_single_instance(const TCHAR *exe)                  /* 在 Processes 里且不在 MultiInstance 里 */
```

两者都用「按 `,` 切分 + `lstrcmp_star`」实现，切分时跳过逗号后空格。

- [ ] **Step 3: 新窗口启发式与重定向**

`is_fresh_app_window(HWND hwnd)` 必须全部满足：`IsWindowVisible`、`GetAncestor(hwnd,GA_ROOT) == hwnd`、`GetWindow(hwnd, GW_OWNER) == NULL`、`!(ExStyle & WS_EX_TOOLWINDOW)`、`Style & WS_CAPTION`、`GetWindowTextLength(hwnd) > 0`、`hwnd != g_hwnd`；Win8+ 再加 `DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))` 得到 0。

`CollectRedirectMaybe(hwnd)` 返回顺序固定：
1. `if (!is_fresh_app_window(hwnd)) return;`
2. `TCHAR exe[MAX_PATH] = {0}; if (!GetWindowProgName(hwnd, exe, ARR_SZ(exe))) return;`
3. `if (!collect_app_matches(collect_processes, exe)) return;`
4. `if (!collect_is_single_instance(exe)) return;`（用户取消了勾选 → 允许并行）
5. 在 `collect_items` 里找 `exe` 相同的一项，找不到就 `return`（应用本来没在托盘里，不干预）。
6. `CollectRestoreIndex(idx);`
7. 把 `hwnd` 放进 `collect_pending_close[]` 第一个空位（满了就覆盖最旧的），`SetTimer(g_hwnd, COLLECT_REDIR_TIMER, 400, NULL);`

`CollectClosePending()`（`WM_TIMER && wParam == COLLECT_REDIR_TIMER` 时调用）：`KillTimer`，遍历队列，仍 `IsWindow()` 的 `PostMessage(hwnd, WM_CLOSE, 0, 0)`，然后清空队列。

`CollectWinEventProc` 里：`EVENT_OBJECT_CREATE` 与 `EVENT_OBJECT_SHOW` → `CollectRedirectMaybe(hwnd)`。

- [ ] **Step 4: 构建并手工验证**

`make -j2`，零 error、零新 warning。ini：

```ini
[TrayCollect]
Enabled=1
Processes=notepad.exe,vscode.exe
MultiInstance=
```

1. 开一个记事本 → 点 X（进托盘）→ 再双击记事本快捷方式 → 已收纳的窗口还原到前台，新窗口在约 0.4 秒内被关掉，最终只剩一个记事本窗口与一个图标。
2. 把 `MultiInstance=notepad.exe` 后重复 1 → 允许两个实例并存，两个都能各自收纳。
3. 开 VS Code（多窗口应用）：勾着「唯一实例」时开第二个窗口 → 记录实际结果。若第二个窗口被关掉，确认这是 spec §7.6 写明的已知代价，并在 Task 7 的记录里写明。
4. 微信/QQ 这类常驻托盘应用：重复 1 → 记录新进程是否真的退出（任务管理器数进程）；不退出时确认界面仍是「回到已有窗口」且无隐形窗口。

- [ ] **Step 5: 提交**

```bash
git add collect.c altsnap.c
git commit -m "feat: 唯一实例——再次启动时回到已收纳窗口"
```

---

### Task 6: 翻译、提示文案与默认 ini 文档

**Files:**
- Modify: `languages.h`（`LANGUAGE_MAP`：`ConfigTabCollect` 放在 `ConfigTabAwake` 之后；`Collect*` 集中放在 Awake 那组之后）
- Modify: `Lang/zh_CN.ini`（`[Translation]` 段）
- Modify: `AltSnap.dni`（`[TrayCollect]` 段）

**Interfaces:**
- Consumes: `LNGVALUE(name, TEXT("English"), TEXT("tooltip"))` 三参数形式、`L10NIDX(name)`、`UpdateDialogStrings`
- Produces: 8 个新键：`ConfigTabCollect`、`CollectBox`、`CollectEnabled`、`CollectListHeader`、`CollectAddBtn`、`CollectDelBtn`、`CollectFindBox`、`CollectHint`

- [ ] **Step 1: 加英文默认值**

在 `languages.h` 的 `LANGUAGE_MAP` 里加（键名与 Task 4 里的 `L10NIDX` 一一对应）：

```c
LNGVALUE(ConfigTabCollect,  TEXT("Tray"), NULL) \
LNGVALUE(CollectBox,        TEXT("Minimize to tray on close"), NULL) \
LNGVALUE(CollectEnabled,    TEXT("&Collect windows of the listed applications to the tray"), NULL) \
LNGVALUE(CollectListHeader, TEXT("&Applications (exe name, * wildcard allowed):"), NULL) \
LNGVALUE(CollectAddBtn,     TEXT("&Add"), NULL) \
LNGVALUE(CollectDelBtn,     TEXT("&Delete"), NULL) \
LNGVALUE(CollectFindBox,    TEXT("Identify &window"), NULL) \
LNGVALUE(CollectHint,       TEXT("Checked = single instance: launching it again brings back the collected window instead of opening a second one.\nWindows of elevated applications cannot be hidden unless AltSnap also runs elevated."), NULL) \
```

- [ ] **Step 2: 中文**

`Lang/zh_CN.ini`（UTF-16LE + BOM）的 `[Translation]` 段加同名键：

```ini
ConfigTabCollect=托盘
CollectBox=关闭时收入托盘
CollectEnabled=将下列应用的窗口收入托盘(&C)
CollectListHeader=应用列表（exe 名，可用 * 通配）：(&A)
CollectAddBtn=添加(&A)
CollectDelBtn=删除(&D)
CollectFindBox=识别窗口(&W)
CollectHint=勾选 = 唯一实例：再次启动会回到已收纳的窗口，而不是开第二个。\n被收纳的应用若以管理员运行，需要 AltSnap 也以管理员运行。
```

- [ ] **Step 3: 默认 ini 注释**

`AltSnap.dni` 末尾加：

```ini
[TrayCollect]
Enabled=0
; 1 = 关闭这些应用的窗口时收进托盘而不是真的关掉。
; Processes 逗号分隔，语法同 [Blacklist] Processes（支持 *.exe）。
Processes=
; 取消勾选“唯一实例”的应用写在这里（默认全部是唯一实例）。
MultiInstance=
```

- [ ] **Step 4: 构建并手工验证**

`make -j2`，零 error、零新 warning。然后：
1. `Language=zh-CN` → 新页签标题是「托盘」，各控件文案、提示框都是中文，无乱码。
2. `Language=en-US` → 全部英文，工具提示（`CreateInfoTip`）不空。
3. 删掉（临时改名）`Lang/zh_CN.ini` 里的 `CollectHint` → 仍显示英文默认值，不崩溃。

- [ ] **Step 5: 提交**

```bash
git add languages.h Lang/zh_CN.ini AltSnap.dni
git commit -m "feat: 托盘收纳页的翻译、提示文案与默认 ini 注释"
```

---

### Task 7: 全清单回归与实测记录

**Files:**
- Create: `docs/superpowers/plans/2026-11-28-collect-to-tray-verification.md`

- [ ] **Step 1: 逐条跑 spec §9 的 17 条验证清单**

按 spec `docs/superpowers/specs/2026-11-28-collect-to-tray-design.md` §9 原文逐条执行，每条记录：通过 / 失败 / 不适用，以及可复现的最小步骤与现象。特别要留证据的三条：Win10 上「拖动一个图标其余不跟随」、强杀后的自动还原、以及「拦不到关闭的程序清单」。

- [ ] **Step 2: 写记录**

新建 `docs/superpowers/plans/2026-11-28-collect-to-tray-verification.md`，结构：测试环境（Windows 版本 / AltSnap 是否提权）→ 17 条结论表 → 「已知缺口实测清单」（自定义标题栏程序、多窗口应用误伤、常驻托盘应用新进程是否退出）→ 未修复项与建议。

- [ ] **Step 3: 提交**

```bash
git add docs/superpowers/plans/2026-11-28-collect-to-tray-verification.md
git commit -m "docs: 托盘收纳功能实测记录"
```

---

## Self-Review

**Spec coverage**：§2 G1→Task 4；G2→Task 1；G3→Task 2；G4→Task 3；G5→Task 1 Step 5 第 5 条；G6→Task 5。§7.1 配置→Task 1/4/5；§7.2 两处拦截→Task 1；§7.3 图标/菜单/清理/持久化→Task 1/2/3，重定向→Task 5；§7.4 设置页→Task 4；§7.5 防线→Task 3；§7.6 缺口→Task 7 记录；§8 文件清单→File Structure；§9 清单→Task 7；i18n→Task 6。无遗漏。

**与 spec 的一处偏差**：spec §7.2 说 Alt+F4 要连 `WM_SYSKEYUP` 一起吞 —— Task 1 Step 3 的代码只对 DOWN 发 `WM_TRAYCOLLECT`、对 UP 只吞不发，避免同一次按键触发两次收纳。这是有意的，实现时按 Task 1 的代码走。

**类型一致性**：`CollectRestoreIndex(int)` / `CollectRemoveIndex(int)` / `CollectFindIndex(HWND)->int` / `CollectWindow(HWND)` / `CollectIconMessage(WPARAM, LPARAM)` / `CollectReloadConfig(void)` / `CollectSaveConfig(void)` 在 Task 1 定义、Task 2/3/4/5 引用，签名一致；`collect_item_t` 在 Task 5 增加 `exe[MAX_PATH]` 字段，这是唯一的结构变更，且只由 Task 5 使用。

**Review Focus 落位**：#1→Task 2 Step 4.6；#2→Task 2 Step 4.4 与 Task 3 Step 4.2；#3→Task 1 Step 5.3；#4→Task 1 Step 5.6；#5→Task 4 Step 6.6。

**比例**：本计划约 400 行，spec 369 行；代码块限于签名、固定顺序的判定逻辑与资源模板，不含可直接推导的函数体。
