# AltSnap「关闭即收入托盘」设计

状态：已批准（2026-11-28）
日期：2026-11-28
参考实现：[lixi523/traynard](https://github.com/lixi523/traynard)（Lazarus / Free Pascal，其核心改造是给每个托盘图标分配唯一 `guidItem`）

## 1. 意图

把 Traynard 的核心能力并入 AltSnap：对用户指定的应用，**关闭窗口时不真关，而是把窗口藏起来并在系统托盘留一个独立图标**，点图标还原。

两个被用户明确选定的点：

1. **每个窗口一个独立托盘图标**。Traynard 存在的全部理由就是这一点：Windows 会把同一进程注册的、没有 `guidItem` 的托盘图标当作一组（相邻排列、整组拖动），所以必须给每个图标唯一 GUID 才能彼此独立。
2. **触发点是"关闭"**，不是"最小化"。点关闭按钮或按 Alt+F4 时拦截，而不是监听最小化事件。

为什么现在做：AltSnap 已经是常驻的窗口操作工具（低级鼠标/键盘钩子 + 单个托盘图标 + 成熟的设置页框架），"关闭到托盘"与它现有的窗口操作同源，不需要引入新进程、新 DLL 注入或新的配置体系。

## 2. 目标

- **G1** 设置里新增一页，可增删“要收纳的应用”（exe 文件名，精确匹配、大小写不敏感；整项写成 `*` 表示所有应用）。
- **G2** 列表内应用的窗口，点关闭按钮或按 Alt+F4 时被隐藏，托盘出现一个**可被系统单独排列**的图标。
- **G3** 单击图标还原对应窗口；图标在窗口还原后**继续保留**（图标代表“该应用已启用收纳”，不是“当前有窗口被隐藏”），只在窗口销毁、右键「关闭」、从列表移除或 AltSnap 退出时才撤掉；右键图标有「还原 / 置顶 / 关闭 / 全部还原」。
- **G4** AltSnap 退出、崩溃或系统重启后，不留下用户无法还原的"隐形窗口"。
- **G5** 未列入列表的应用，行为与现在完全一致。
- **G6** 每个列入的应用带一个「唯一实例」开关（默认勾选）：勾选时，若该应用已有窗口在托盘，再次启动它不会出现第二个窗口，而是把托盘里那个还原并置前，新窗口被关掉；取消勾选则允许并行多开。

## 3. 非目标

- 不做"最小化即收纳"（用户明确只需关闭时）。
- 不做 Traynard 的按应用分组图标、规则引擎（标题/类名/正则）、可配置热键、气泡通知、开机计划任务。
- 不做全局 DLL 注入（见 §6 方案 B），除非方案 A 实测证明不够。
- 不改 `AC_*` 动作表，不加"手动收入托盘"的动作或热键。
- 不做多实例（`-m`）之间的状态协调。

## 4. 反目标

- **吞掉关闭动作却没成功隐藏**。用户会以为窗口关了，实际变成一个既不可见又不可还原的僵尸。必须有回退路径（§7.3 第 3 步）。
- **让 hooks.dll 的缺陷波及非 AltSnap 进程**。任何把 hooks.dll 变成全局注入 DLL 的做法都直接违反这条。
- **在鼠标钩子热路径里做重活**。hooks.c 的 `LowLevelMouseProc` 每次点击都会被调用，不能无条件加跨进程 `OpenProcess` + `GetWindowText`。

## 5. 约束

- **架构规则**（沿用 `awake.c` 顶部那条，同一道理）：托盘状态、窗口枚举、配置写入必须在主程序侧；`hooks.c` 只负责"检测 + 吞掉输入 + 通知主程序"。`hooks.dll` 当前只在 AltSnap 进程内运行（`WH_MOUSE_LL` / `WH_KEYBOARD_LL` 的回调在本进程），**不向其他进程注入**。
- 源码 C99（`Makefile: CFLAGS = -Os -std=c99`），可用现有宏：`ARR_SZ`、`mem00`、`pure` / `xpure`、`V`（`(void*)` 默认值转换）、`LOG`。
- ANSI 与 Unicode 两种构建都要能编译：一律用 `TCHAR` / `lstr*` 系列，不写死 `char` / `wchar_t`。
- 资源路径：`altsnap.rc` → `window.rc` 对话框模板，ID 在 `resource.h`，资源 `.o` 由 Makefile 依赖自动重建。
- 翻译键加在 `languages.h` 的 `LANGUAGE_MAP`（同一条宏同时生成 `struct strings`、ini 键名数组、英文默认值），中文补进 `Lang/zh_CN.ini` 的 `[Translation]`；缺键自动回退英文，不会破坏现有翻译。
- 多套 Makefile（`Makefile` / `MakefileX64` / `MakefileARM64Docker` 等）：新模块按 `awake.c` 的方式被 `altsnap.c` 直接 `#include`，不新增编译单元，Makefile 只加依赖项。
- `Shell_NotifyIcon` 走 `<shellapi.h>`（经 `windows.h` 引入）；`guidItem` 属于 Vista+ 的 V3 结构，`tray.c` 为了 NT4 兼容自己定义了最小结构，本项目沿用这个做法。

## 6. 候选方案

| | 做法 | 覆盖 | 代价 | 结论 |
| --- | --- | --- | --- | --- |
| **A** | 输入层拦截：`LowLevelMouseProc` 判定点击落在 `HTCLOSE` 就吞掉；`LowLevelKeyboardProc` 吞 Alt+F4 | 点 X、Alt+F4 | 任务栏右键关闭、程序自身"退出"菜单仍会真关 | **选定** |
| B | 让 hooks.dll 变成全局 `WH_CBT` 钩子，吃掉 `HCBT_SYSCOMMAND` / `SC_CLOSE` | 全部关闭路径 | Windows 会把 hooks.dll 注入每个 GUI 进程；DLL 缺陷可崩别人的程序；杀软敏感；提权进程进不去；三套平台各一份 | 否决 |
| C | 改为"最小化即收纳"（`EVENT_SYSTEM_MINIMIZESTART`） | 零缺口 | 不是用户要的语义 | 只作退路 |

选 A 的理由：它不改变 hooks.dll 的性质（仍只在 AltSnap 进程内），复用已有的钩子与命中测试设施（`HitTestTimeoutbl` 已在 `InTitlebar` 里用来区分 `HTCLOSE` 等标题栏按钮），失败面局限在"某些程序的关闭按钮拦不到"，而这类缺口可枚举、可记录、不影响其他应用。

## 7. 设计

### 7.1 配置

ini 新增一段，三个用户键 + 一个运行时键：

```ini
[TrayCollect]
Enabled=0
; 逗号分隔，语法与 [Blacklist] Processes 完全一致
Processes=notepad.exe,wechat.exe
; “唯一实例”被取消勾选的应用（默认全部勾选，所以这里记例外）
MultiInstance=notepad.exe
; 运行时写入，用于崩溃后找回
Windows=
```

- `Processes` 的匹配语义**直接继承** `[Blacklist] Processes`：逗号分隔、逗号后空格跳过。注意：`blacklist_t` 里 **exe 名是精确（大小写不敏感）比较，不支持 `*.exe` 通配**（通配只对 title/class 生效）；单独一项 `*` 会被解成“任意 exe”，两侧行为已对齐。hooks.dll 侧复用 `hooks.c` 的 `readblacklist()`；主程序侧用 `collect_app_matches()` 按同一规则匹配。
- `MultiInstance` 存的是**例外项**而不是"勾选集合"：新加入 `Processes` 的应用自动就是"唯一实例"，不需要在两个列表间做同步；从 `Processes` 删掉的应用会在 `MultiInstance` 里留一条废条目，无害且与其他列表键行为一致。
- `Enabled=0` 时整条链路短路，零开销。

### 7.2 hooks.dll 侧（`hooks.c`）

**数据结构与读取**

```c
static blacklist_t CollectList;   /* 新；与 BlkLst 各成员同类型 */
```

新增读取函数，放在 `readallblacklists()`（hooks.c:6413）之后：

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

在 `Load()`（hooks.c:6652）里 `readallblacklists(inipath)`（hooks.c:6787）之后调用一次。设置页 Apply 会走 `UpdateSettings()` → `UnhookSystem()` + `HookSystem()` → `Load()` 重读，**不需要额外通道**。

**检测点一：关闭按钮**（`LowLevelMouseProc`，hooks.c:5437）

插入位置：`enum button button = ...` 之后、`TitleBarActions` 调用之前。

```c
if (CollectList.length && (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP)) {
    if (collect_close_click(pt, wParam == WM_LBUTTONUP))
        return 1;   /* 吞掉，窗口收不到这次点击 */
}
```

`collect_close_click()` 的判定顺序（便宜的先做，避免给所有应用付代价）：

1. `HWND hwnd = WindowFromPoint(pt);` 空则返回 0。
2. `HWND root = GetAncestor(hwnd, GA_ROOT);` 空、或等于 `g_mainhwnd` 则返回 0。
3. `TCHAR exename[MAX_PATH] = {0}; GetWindowProgName(root, exename, ARR_SZ(exename));` 取不到就返回 0。
4. 单元素缓存（`static HWND cached_hwnd; static int cached_hit;`）：`root` 未变时复用上次结果，否则 `cached_hit = blacklisted_from_names(&CollectList, exename, NULL, NULL)`。**只按 exe 名匹配**，标题/类名传 `NULL`（`lstrcmp_star()` 对 `b == NULL` 直接返回匹配，见 `nanolibc.h:420`）。
5. `cached_hit` 为 0 则返回 0。
6. `int area = HitTestTimeoutL(hwnd, MAKELPARAM(pt.x, pt.y));` 不等于 `HTCLOSE` 则返回 0。
7. 记录 `collect_pending_hwnd = root`，`PostMessage(g_mainhwnd, WM_TRAYCOLLECT, (WPARAM)root, 0)`，返回 1。
8. 抬起时若 `root == collect_pending_hwnd`：清空该变量并返回 1。

**为什么不用 `HitTestTimeoutbl()` / `blacklisted()`**：这两个函数内部会调 `GetWindowText()`，那是跨进程**同步**消息、没有超时。低级钩子里一旦阻塞在目标线程上就会冻结整个输入流，而目标线程很可能正等着输入——经典死锁。而 `HitTestTimeoutL()` 用的是 `SendMessageTimeout(..., 255ms)`（`unfuck.h:1564`），`GetWindowProgName()` 走 `OpenProcess` + `GetModuleFileNameEx`，都不发窗口消息。**本条路径里禁止出现 `GetWindowText` / `HitTestTimeoutbl` / `blacklisted`。** 代价是失去了 `HitTestTimeoutbl` 里的根窗口回退（部分 Win10 应用的 `WM_NCHITTEST` 只在父窗口正确），记入 §7.6 已知缺口。

为什么按下和抬起都要吞：Windows 的默认按钮行为是"按下被接收 → 抬起时窗口自己发 `WM_SYSCOMMAND, SC_CLOSE`"。只吞按下不够。

拦截位置在 `TitleBarActions` 调用之前，因此当用户把标题栏动作映射到 Close 时，**收纳优先**（先被我们吞掉）。

**检测点二：Alt+F4**（`LowLevelKeyboardProc`，hooks.c:2744）

在该回调处理 `WM_KEYDOWN` / `WM_SYSKEYDOWN` 的位置加：

```c
if (CollectList.length && wParam == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) {
    HWND root = GetAncestor(GetForegroundWindow(), GA_ROOT);
    TCHAR exename[MAX_PATH] = {0};
    if (root && GetWindowProgName(root, exename, ARR_SZ(exename))
    &&  blacklisted_from_names(&CollectList, exename, NULL, NULL)) {
        PostMessage(g_mainhwnd, WM_TRAYCOLLECT, (WPARAM)root, 0);
        return 1;
    }
}
```

同样**只用 `GetWindowProgName` + exe 名匹配**，理由同上（键盘钩子里同步等待目标线程等于自杀）。

同时吞 `WM_SYSKEYUP` 的同一组合（否则系统可能仍走关闭路径，实测确认）。

**新增消息**（`hooks.h`，`WM_GETZONES` = `WM_USER+20` 之后）：

```c
#define WM_TRAYCOLLECT     (WM_USER+21)   /* hooks.dll -> 主程序：请收纳该窗口 */
#define WM_TRAYCOLLECTMSG  (WM_USER+22)   /* Shell_NotifyIcon 的回调消息 */
```

只需两个：WinEvent 的回调跑在主线程上（见 §7.3），不需要额外的“窗口已销毁”消息。

### 7.3 主程序侧（新增 `collect.c`）

`collect.c` 由 `altsnap.c` 以 `#include "collect.c"` 引入，位置在 `#include "awake.c"` 之后、`#include "tray.c"` 之前（它要用 `g_hwnd` / `g_hinst` / `inipath` / `WinVer` / `WM_TASKBARCREATED`，这些都在 `altsnap.c` 顶部或其之前定义）。

**状态**：定长数组，不做堆管理。

```c
#define COLLECT_MAX 32
typedef struct {
    HWND  hwnd;
    DWORD pid;
    UINT  id;        /* NOTIFYICONDATA.uID，同时用于撤图标 */
    GUID  guid;
} collect_item_t;
static collect_item_t collect_items[COLLECT_MAX];
static int collect_count;
static UINT collect_nextid = 1;
static UINT collect_cbmsg = WM_TRAYCOLLECTMSG;   /* 固定消息号，必须与 WindowProc 的分支一致 */
```

**结构与职责**

- `CollectLoadConfig()` — 读 `[TrayCollect] Enabled`；读 `Windows=` 并按 §7.5 处理。
- **主程序侧不再二次判断 `Enabled`**：`CollectWindow()` 只信任 hooks.dll 的通知。两侧各自读 ini，一旦把开关做两次判断，就可能出现"hooks 已经吞掉点击、主程序却拒绝收纳"的僵尸窗口。`Enabled` 只影响 hooks.dll 是否装载列表。
- `CollectWindow(HWND hwnd)` — 由 `WM_TRAYCOLLECT` 调用：
  1. 已在本表 → `ShowWindow(hwnd, SW_HIDE)` 后返回（图标已在，复用，不重复创建）。
  2. 取窗口图标：`WM_GETICON(ICON_BIG)` → `WM_GETICON(ICON_SMALL2)` → `GCLP_HICON` → `GCLP_HICONSM` → 兜底 `LoadIcon(g_hinst, MAKEINTRESOURCE(APP_ICON))`。
  3. `ShowWindow(hwnd, SW_HIDE);` 然后 `if (IsWindowVisible(hwnd)) { PostMessage(hwnd, WM_CLOSE, 0, 0); return; }` — **反目标守卫**：藏不住就把用户原本的关闭意图执行掉，绝不制造僵尸窗口（提权窗口走这条）。
  4. 组通知结构。**自定义一份 V3 结构**（照 `tray.c` 的做法，不依赖 `NOTIFYICONDATAW` 这个名字，因为同一份源码要同时支持 A/W 构建）：

     ```c
     typedef struct {
         DWORD cbSize; HWND hWnd; UINT uID; UINT uFlags; UINT uCallbackMessage;
         HICON hIcon; TCHAR szTip[128]; DWORD dwState, dwStateMask;
         TCHAR szInfo[256]; UINT uVersion; TCHAR szInfoTitle[64];
         DWORD dwInfoFlags; GUID guidItem; HICON hBalloonIcon;
     } collect_nid_t;
     ```

     填 `hWnd = g_hwnd`、`uID = collect_nextid++`、`uFlags = NIF_MESSAGE|NIF_ICON|NIF_TIP`（`WinVer >= VISTA` 时再加 `NIF_GUID`）、`uCallbackMessage = collect_cbmsg`、`szTip` = 窗口标题（截断到 127）、`guidItem = CollectGuid(exename, hwnd)`。`cbSize` 用上面这个结构的尺寸；`NIM_ADD` 失败时退到 `tray.c` 那种最小结构（只有 `szTip[64]`、无 `guidItem`）重试一次，兼容旧系统。
  5. `Shell_NotifyIcon(NIM_ADD)`（失败重试 3 次，间隔 100ms，与 `tray.c:UpdateTray` 同款容错）；彻底失败则 `ShowWindow(hwnd, SW_SHOW)` 回滚并 `LOG`。
  6. 记录 `pid`、`CollectSaveConfig()`。
- `CollectGuid(exe, hwnd)` — 用 FNV-1a 对 `"AltSnap/Collect/<exe>/<hwnd>"` 派生 GUID（第 7 字节置 4、第 9 字节置 `10xx` 变体）。**每个窗口必须不同**：GUID 相同会被 Windows 当成同一个图标。
- `CollectRestore(int i)` — 只 `ShowWindow(hwnd, SW_SHOW)` + `SetForegroundWindow(hwnd)`；**不撤图标、不移出表**（G3：图标常驻，之后再点 X 复用同一图标）。
- `CollectRemove(int i)` — 撤图标 + 移出表，不动窗口（窗口已自己销毁、右键「关闭」、从列表移除时用）。
- 图标回调（`WM_TRAYCOLLECTMSG`，即 `WM_USER+22`）：
  - `WM_LBUTTONUP` / `WM_LBUTTONDBLCLK` → `CollectRestore`
  - `WM_RBUTTONUP` → 右键菜单：`SetForegroundWindow(g_hwnd); PostMessage(g_hwnd, WM_NULL, 0, 0);` 再 `TrackPopupMenu`（托盘菜单的标准做法，`tray.c:ShowContextMenu` 同款），菜单项「还原 / 置顶 / 关闭 / — / 全部还原」。菜单命令用 `CreatePopupMenu` 的返回 id 映射到表下标。
  - 不调用 `NIM_SETVERSION`：沿用老式 `WM_*` 回调，与现有 `tray.c` 一致，少一个平台差异面。
- 窗口销毁：在同一个 `WinEventProc` 里直接处理（`EVENT_OBJECT_DESTROY`，回调本身就在主线程，见下），`NIM_DELETE` + 移除条目，不需要跨线程中转。
- `CollectReAddAll()` — `WM_TASKBARCREATED` 时逐条 `NIM_ADD`（explorer 重启后图标要回来）。
- `CollectShutdown()` — `WM_DESTROY` 时：全部 `NIM_DELETE`、全部 `ShowWindow(SW_SHOW)`、清 `Windows=`。

`collect_item_t` 需要多一个 `TCHAR exe[MAX_PATH]` 字段，因为“唯一实例”判定要按 exe 名比对（定长数组下约占 16KB，可接受）。

**唯一实例重定向（`MultiInstance` 未列出的应用）**

这是 G6 的全部实现，纯主程序侧、零注入。复用同一个 `SetWinEventHook`：

```c
SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, NULL, CollectWinEventProc,
                0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
```

`WINEVENT_OUTOFCONTEXT` 的回调由**安装钩子的线程**（即主线程）在它的消息循环里派发，所以这里不需要 `PostMessage` 中转，可以直接改表——同理，`EVENT_OBJECT_DESTROY` 也直接处理，原设计里那个 `WM_TRAYCOLLECTGONE` 消息可以删掉。

`CollectWinEventProc()` 内先做通用过滤：`idObject == OBJID_WINDOW && idChild == CHILDID_SELF`，且 `hwnd` 本身是顶层（`GetAncestor(hwnd, GA_ROOT) == hwnd`）。然后分事件：

- `EVENT_OBJECT_DESTROY` → 若在本表里，`NIM_DELETE` + 移除条目。
- `EVENT_OBJECT_CREATE` / `EVENT_OBJECT_SHOW` → 若 `hwnd` 已在本表，直接返回（这是我们收纳过的窗口重新显示，不是新实例；图标保留）；否则 `CollectRedirectMaybe(hwnd)`。

`is_fresh_app_window(hwnd)` 启发式（全部满足才算“真的冒出一个新实例”）：

| 条件 | 目的 |
| --- | --- |
| `IsWindowVisible(hwnd)` | 过滤隐藏/预创建窗口 |
| `GetWindow(hwnd, GW_OWNER) == NULL` | 排除对话框、子面板 |
| `!(ExStyle & WS_EX_TOOLWINDOW)` | 排除工具窗口 |
| `Style & WS_CAPTION` | 排除无标题栏的辅助窗口 |
| `DwmGetWindowAttributeL(hwnd, DWMWA_CLOAKED) == 0`（Win8+） | 排除 UWP 影子窗口 |
| `GetWindowTextLength(hwnd) > 0` | 排除空标题窗口（该 API 对其他进程只读缓存，不发消息，钩子安全） |
| `hwnd != g_hwnd` | 不碰自己 |

`CollectRedirectMaybe(hwnd)` 流程：

1. 取 `exename`；不在 `Processes` 命中或命中不到，返回。
2. 在 `MultiInstance` 里命中（用户取消了勾选）→ 返回，允许并行多开。
3. 在本表里找一个 `collect_item_t` 且其 `exe` 与 `exename` 相同 → 没有则返回（应用本来就没被收纳，不干预）。
4. 该项的 `pid` 与 `hwnd` 的进程相同 → 返回（同进程的另一个窗口不是新实例，关掉会丢用户数据）。
5. `CollectRestore(idx)` 先还原已有窗口（它会置前，新窗口自然失去前台；图标保留）。
6. 把 `hwnd` 放进 `collect_pending_close[]`（定长 4），`SetTimer(g_hwnd, COLLECT_REDIR_TIMER, 400, NULL)`；定时器到期时对仍 `IsWindow()` 的项 `PostMessage(hwnd, WM_CLOSE, 0, 0)`，然后清空。
   延迟 400ms 是必要的：`EVENT_OBJECT_CREATE` 往往在窗口完成初始化之前到达，立即发 `WM_CLOSE` 可能被应用忽略或导致它重建。
7. `LOG` 记一行（exe 名 + 新旧 hwnd），方便验证时统计误判。

**接线（`altsnap.c` 的 `WindowProc`）**

```c
} else if (msg == WM_TRAYCOLLECT) {
    CollectWindow((HWND)wParam);
} else if (msg == WM_TRAYCOLLECTMSG) {
    CollectIconMessage(wParam, lParam);   /* 按 lParam 的小写 word 分发 */
} else if (msg == WM_TIMER && wParam == COLLECT_REDIR_TIMER) {
    CollectClosePending();
}
```

另外：`InitTray()` 之后调 `CollectInit()`（`collect_cbmsg = WM_TRAYCOLLECTMSG`、读配置、装 §7.3 的 `SetWinEventHook`（`EVENT_OBJECT_CREATE..EVENT_OBJECT_SHOW`））；`WM_TASKBARCREATED` 分支追加 `CollectReAddAll()`；`WM_DESTROY` 分支追加 `CollectShutdown()`。

`hooks.h` 只需加两个消息号：`WM_TRAYCOLLECT`（hooks.dll → 主程序）与 `WM_TRAYCOLLECTMSG`（`Shell_NotifyIcon` 回调）。同进程通信，不需要 `ChangeWindowMessageFilter`。

### 7.4 设置页

- `resource.h`：`#define IDD_COLLECTPAGE 209`；控件 ID 用 2300 段（2200 段已被 Awake 占用）：`IDC_COLLECT_BOX 2300`、`IDC_COLLECT_ENABLED 2301`、`IDC_COLLECT_LIST 2302`、`IDC_COLLECT_ADD 2303`、`IDC_COLLECT_FIND 2304`、`IDC_COLLECT_HINT 2305`、`IDC_COLLECT_EDIT 2306`、`IDC_COLLECT_DEL 2307`。
- `window.rc`：新增 `IDD_COLLECTPAGE DIALOG DISCARDABLE 0, 0, 220, 190`，样式与 `IDD_AWAKEPAGE` 一致：
  - `GROUPBOX "" ,IDC_COLLECT_BOX, 3,1,212,186`
  - `AUTOCHECKBOX "",IDC_COLLECT_ENABLED, 10,10,198,8`
  - `LTEXT ""`（列表标题）
  - `CONTROL "",IDC_COLLECT_LIST,"SysListView32",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP|WS_BORDER, 10,22,200,86`
  - 下一行：`ICON IDI_FIND, IDC_COLLECT_FIND, ..., SS_NOTIFY|SS_REALSIZEIMAGE` + `EDITTEXT IDC_COLLECT_EDIT` + `PUSHBUTTON`（添加，`IDC_COLLECT_ADD`）+ `PUSHBUTTON`（删除，`IDC_COLLECT_DEL`）
  - `LTEXT ""`（`IDC_COLLECT_HINT`）。
- **控件选型（我的判断）**：用 `SysListView32` + `LVS_EX_CHECKBOXES`，因为“每个应用一个勾选框”用 ListView 最直接；`config.c` 已经 `#include <commctrl.h>` 并用着 comctl32（`PropSheet_*` / `TabCtrl_*`），不引入新依赖。不要用“一个复选框绑定到选中行”的方案：那会多一个“当前行”的隐藏状态。
- `config.c`：
  - `pages[]`（config.c:125）在 `IDD_AWAKEPAGE` 之前插入 `{ IDD_COLLECTPAGE, (DLGPROC)CollectPageDialogProc }`。
  - `UpdateStrings()`（config.c:180）：`const TCHAR *titles[7]` 改为 `titles[8]`，插入 `l10n->ConfigTabCollect`。**这是最容易漏的一处**：数组长度和下面的 `ARR_SZ(titles)` 循环必须同步。
  - `CollectPageDialogProc`：
    - `optlst` 只剩一条：`{ IDC_COLLECT_ENABLED, T_BOL, 0, TEXT("TrayCollect"), "Enabled", V 0 }`（`ReadDialogOptions` / `WriteDialogOptions` 直接复用）。列表**不走 optlst**：ListView 无法用 `ReadOptionStrW` 那种单字符串映射。
    - `WM_INITDIALOG`：`ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT)`，`ListView_InsertColumn(list, 0, LVCF_TEXT, "", 190)`（列标题用 `LVCF_TEXT` 但文本留空，避免多一条翻译键）。
    - 两个小助手：`CollectListAdd(hwnd, name, checked)`（去重后 `ListView_InsertItem` + `ListView_SetCheckState`）、`CollectListClear(hwnd)`。
    - 读配置（`WM_INITDIALOG` / `PSN_SETACTIVE`）：`GetPrivateProfileString(TEXT("TrayCollect"), TEXT("Processes"), ...)` 取整串，按 `,` 切分逐项 `CollectListAdd(hwnd, name, 1)`；再取 `MultiInstance` 整串，对命中的项 `ListView_SetCheckState(item, FALSE)`。没有“勾选集合”这个键，因为 `MultiInstance` 只存例外（§7.1）。
    - 添加 / 删除：添加取 `IDC_COLLECT_EDIT` 内容（去重）；删除取当前选中项。
    - 勾选变化：ListView 不会产生 `EN_UPDATE`，所以必须在 `WM_NOTIFY` 里接 `LVN_ITEMCHANGED`（`uChanged & LVIF_STATE`）→ `PropSheet_Changed(g_cfgwnd, hwnd)`，否则用户只勾复选框时 **Apply 按钮不会亮**，改动会静默丢失。
    - `PSN_APPLY`：遍历项，命名的拼成 `Processes`，`ListView_GetCheckState(item) == 0` 的再拼成 `MultiInstance`，分别 `WritePrivateProfileString`；`Enabled` 由 `WriteDialogOptions` 写。然后 `CollectReloadConfig()` + `UpdateSettings()`（让 hooks.dll 重读列表）。
  - 复用“找窗口”准星：`FindWindowProc`（config.c:1758 附近）里加一段——若当前页存在 `IDC_COLLECT_EDIT`，把 `GetWindowProgName()` 得到的 exe 名写进去；黑名单页原有字段逻辑不动。
- i18n：`languages.h` 的 `LANGUAGE_MAP` 增加 `ConfigTabCollect`、`CollectBox`、`CollectEnabled`、`CollectListHeader`、`CollectAddBtn`、`CollectDelBtn`、`CollectFindBox`、`CollectHint`；`Lang/zh_CN.ini` 补对应中文。
- `AltSnap.dni`：增加 `[TrayCollect]` 段与注释，让新用户知道有这功能。

### 7.5 持久化与"隐形窗口"防线

- 每次收纳 / 移除条目时重写 `[TrayCollect] Windows=hwnd:pid,...`（还原窗口不重写：图标与记录常驻）。
- **启动时**（`CollectInit`）：逐条校验 `IsWindow(hwnd)` 为真、`GetWindowThreadProcessId == pid`、且 `!IsWindowVisible(hwnd)` → `ShowWindow(hwnd, SW_SHOW)`；然后清空该键。
- **决定（我的判断，请核对）**：启动时一律"还给用户"，不重建图标。重建图标看着更聪明，但只要恢复入口在启动瞬间丢一次，那个窗口就再也找不回来了；安全优先。
- AltSnap 正常退出：还原所有窗口（§7.3 `CollectShutdown`），不留"退出后窗口消失"的口实。

### 7.6 边界与已知缺口

- **提权窗口**：低级钩子能收到并能吞掉点击（LL 钩子不受 UIPI 限制），但 `ShowWindow` 在非管理员 AltSnap 里对提权进程的窗口可能失败。此时 §7.3 第 3 步的回退会真的把它关掉——符合用户意图，不产生僵尸窗口。页面提示里写明"被收纳的应用以管理员运行时，AltSnap 也需以管理员运行"。
- **AltSnap 暂停时**（托盘菜单 Disable / `UnhookSystem`）：钩子被卸载，关闭拦截随之失效，此时关闭窗口会真关。这是现有架构的必然结果，写进页面提示。
- **自定义标题栏**（部分 Electron / 游戏）`WM_NCHITTEST` 不返回 `HTCLOSE`：点 X 拦不到，会真关。先用 `HTCLOSE` 一条判定，`DWMWA_CAPTION_BUTTON_BOUNDS` 右 1/3 的兜底**暂不做**（YAGNI），实测确有需要再加。
- **不用 `HitTestTimeoutbl` 带来的覆盖损失**：它是带根窗口回退的版本，部分 Win10 应用的 `WM_NCHITTEST` 只在父窗口正确。我们为避开钩子里的同步 `GetWindowText` 而改用 `HitTestTimeoutL`，可能在这类程序上拦不到——与上一条一起记入验证清单。若实测损失明显，改进方向是给命中测试单独加一层"只对祖先窗口发超时消息"，**不是**换回 `HitTestTimeoutbl`。
- **owner 弹窗不处理**：只对 `GetAncestor(hwnd, GA_ROOT)` 生效，被隐藏主窗口的 owned 对话框（子窗、浮动面板）不会被隐藏，也不会单独生成图标。
- **其他关闭路径**（任务栏右键关闭、程序自身退出菜单、别的进程发 `WM_CLOSE`）会真关，这是方案 A 的已知边界。
- **UWP / `ApplicationFrameHost`**：`GetWindowProgName()` 已能穿透到真实进程；图标取不到时回退 `APP_ICON`，不额外特殊处理。
- **多实例（`-m`）**：`Windows=` 键会被互相覆盖。已知限制，不做协调。
- **窗口数上限** 32（`COLLECT_MAX`），超出时 `LOG` 并放弃收纳（此时不吞点击，窗口正常关闭）。
- **「唯一实例」会误伤“第二个正规窗口”**：Chrome/Electron/IDE 这类应用会为每个文档或渲染进程建窗口。启发式能滤掉消息窗口、隐藏窗、工具窗，但“同一 exe 的第二个真正可见顶栏窗口”会被当成新实例关掉，**可能丢掉用户刚写的东西**。缓解：只在“托盘里已有同 exe 窗口”时介入（应用正常多开时我们完全不管）；取消勾选即可放开；页面提示要写明这条。这是 G6 选定语义（§6 方案 1）的固有代价。
- **新进程可能不退出**：常驻托盘类应用收到 `WM_CLOSE` 只隐藏窗口，进程留在后台。不会造成错误状态，但“多了一个进程”会与用户直觉不符，记入验证清单观察。
- **重定向只看“冒出新窗口”**：新实例启动后长时间不建窗口（登录中、离线模式、只建托盘图标）时不会触发，属于已知不覆盖路径。

## 8. 涉及文件

| 文件 | 改动 |
| --- | --- |
| `hooks.h` | `WM_TRAYCOLLECT` / `WM_TRAYCOLLECTMSG` |
| `hooks.c` | `CollectList` + `readcollectlist()` + `Load()` 调用；`LowLevelMouseProc` 关闭按钮拦截；`LowLevelKeyboardProc` Alt+F4 拦截 |
| `collect.c`（新） | 托盘图标、还原、右键菜单、销毁清理、持久化、唯一实例重定向 |
| `altsnap.c` | `#include "collect.c"`；`WindowProc` 三个分支；`CollectInit()`；`WM_TASKBARCREATED` / `WM_DESTROY` 挂接 |
| `resource.h` | `IDD_COLLECTPAGE`、`IDC_COLLECT_*` |
| `window.rc` | `IDD_COLLECTPAGE` 模板 |
| `config.c` | `pages[]`、`titles[]`、`CollectPageDialogProc`（含 ListView 逐项勾选）、`FindWindowProc` 加一段 |
| `languages.h` / `Lang/zh_CN.ini` | 新翻译键与中文 |
| `Makefile*` | `AltSnap.exe:` 依赖加 `collect.c` |
| `AltSnap.dni` | 新增 `[TrayCollect]` 段与注释 |

## 8.5 已知风险与缓解

| 风险 | 缓解 |
| --- | --- |
| 低级钩子里同步等待目标线程 → 冻结输入 / 死锁 | 钩子路径只允许 `SendMessageTimeout`（`HitTestTimeoutL`，255ms）与消息无关 API（`GetWindowProgName`）；明令禁止 `GetWindowText` / `HitTestTimeoutbl` / `blacklisted` |
| 吞了关闭却没藏住（提权/UIPI） | `IsWindowVisible` 校验后回退为真关（§7.3 第 3 步） |
| hooks.dll 崩溃拖垮 AltSnap | 新增代码量极小（一个列表 + 两处判定），不引入新线程、不分配生命周期对象 |
| 列表配错导致误吞 | 只在 `[TrayCollect] Enabled=1` 且 `Processes` 非空时生效；空列表时钩子分支零开销 |
| 「唯一实例」误关用户的新窗口 | 启发式过滤 + 仅在托盘已有同 exe 窗口时介入 + 可取消勾选；页面提示写明会干预多窗口应用 |

## 9. 验证清单（每条都要能手动复现）

1. `[TrayCollect]` 为空时，关闭任意窗口行为不变（回归）。
2. 加入 `notepad.exe`：开记事本 → 点 X → 窗口消失、托盘多一个记事本图标；点图标 → 窗口回来，**图标仍在**；再点 X → 窗口再次消失，图标不重复。
3. 同一应用开 3 个窗口 → 3 个独立图标；Win10 上拖动其中一个，其余不跟随（Traynard 的核心诉求）。Win11 ≥ 22563 改测"每个图标可单独显隐"。
4. Alt+F4 同样收纳。
5. 右键图标 → 还原 / 置顶 / 关闭 / 全部还原 全部生效。
6. 未列入列表的 `mspaint.exe` 点 X 正常关闭。
7. 收纳后任务管理器重启 `explorer.exe` → 图标自动回来。
8. 收纳后正常退出 AltSnap → 窗口全部还原、无残留图标。
9. 收纳后强杀 AltSnap → 重开 AltSnap → 窗口自动还原（不产生隐形窗口）。
10. 以管理员运行的程序（如管理员 PowerShell）：记录实际表现，确认是"真的关掉"而不是变成隐形窗口；再以管理员运行 AltSnap 重复一次。
11. 记录拦不到关闭的自定义标题栏程序清单（作为方案 A 的已知缺口证据）。
12. 收纳一个带 owned 对话框的程序（如 Visual Studio 的浮动面板、记事本的查找框）：确认主窗口被收纳，弹窗行为符合 §7.6 的记录。

## 10. 留给 builder 的判断

- `HitTestTimeoutbl()` 在钩子里的耗时是否可接受；若不可接受，先用 `GetCaptionButtonsRect()`（DWM 标题栏按钮矩形）做便宜的预过滤再发 `WM_NCHITTEST`。
- `COLLECT_MAX = 32` 是否够用。
- 焦点/激活细节：还原时是否需要在 `SetForegroundWindow` 前后加 `AttachThreadInput` 之类的老问题处理（AltSnap 已有 `SetForegroundWindowL`，优先复用）。
- `NIM_ADD` 失败后的最小结构回退是否真的需要（若实测 Vista+ 一次成功，可删掉这条回退）。
- 重定向的延迟关闭阈值（本设计取 400ms）与 `collect_pending_close` 队列长度（取 4）的实测标定；单应用连续快速启动多次时是否会丢事件。
- `is_fresh_app_window()` 各条件的取舍：实测中哪些条件在主力应用上误杀最多，可单独放宽或收紧（例如去掉 `WS_CAPTION` 以覆盖无边框窗口）。

## 11. 修订记录（实现/验收阶段）

以下偏差已在 `collect.c` / `hooks.c` 中修正，本文档已同步：

1. **托盘图标改为常驻**（§G3、§7.3）：原设计“还原即撤图标”。用户实测后明确语义为“图标代表该应用已启用收纳”，因此 `CollectRestore` 只显示窗口，不撤图标、不移出表；条目只在窗口销毁 / 右键「关闭」/ 移出列表 / AltSnap 退出时删除。
2. **点 X 需要常驻鼠标钩子**（§7.2）：`LowLevelMouseProc` 平时只在 Alt 按下时安装，默认配置下点 X 根本不经过钩子。`conf.keepMousehook` 计算时追加 `|| CollectList.length`。
3. **图标回调消息号**（§7.3）：`RegisterWindowMessage` 返回的动态 id 与 `WindowProc` 里的 `WM_TRAYCOLLECTMSG` 永不相等，改为固定使用 `WM_TRAYCOLLECTMSG`。
4. **同进程窗口不再被当新实例**（§7.3 重定向第 4 步）：同 `pid` 的另一个窗口会触发唯一实例逻辑并被 `WM_CLOSE`，加上 pid 判断。

