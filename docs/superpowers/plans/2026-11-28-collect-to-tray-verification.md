# 关闭即收入托盘 — 实测记录

**状态：** 代码已完成（Task 1–6），**Task 7 的交互验收尚未执行**。
GUI 行为（点托盘图标、拖图标、重启 explorer、数进程）必须在真机上由人操作，本环境只能完成编译与静态核对。

日期：2026-11-28
分支：`feat/tray-collect`
Spec：`docs/superpowers/specs/2026-11-28-collect-to-tray-design.md`

## 环境

| 项 | 值 |
| --- | --- |
| 构建工具链 | 仓库自带 `tools/w64devkit`（`gcc` + `make`） |
| 构建命令 | `make -fMakefileX64 -j2`（**不是** `make`，见下） |
| 产物 | `AltSnap.exe`、`hooks.dll` 均为 x86-64 |
| 测试对象 | 待填（Windows 版本 / 是否管理员运行） |

## 已完成（机械可验证）

| 检查 | 结果 |
| --- | --- |
| `make -fMakefileX64 -j2` | `exit=0` |
| 新增编译警告 | 0（仅剩既有的 `nanolibc.h:367 -Wchar-subscripts`） |
| 32 位 `Makefile` | **不可用**：工作区既有的 `altsnapr.o` 为 x86-64 COFF，链接报 `incompatible with i386 output`。若需 32 位，先删 `altsnapr.o` 后重编 |
| 静止代码路径开销 | `Enabled=0` 或 `Processes` 为空时，鼠标/键盘钩子分支在 `CollectList.length` 处短路 |
| 钩子热路径安全 | 只用 `GetWindowProgName`（OpenProcess）与 `HitTestTimeoutL`（`SendMessageTimeout` 255ms）；无 `GetWindowText` / `HitTestTimeoutbl` / `blacklisted` |

## 2026-11-28 实机验证（已执行）

验证对象：`D:\操作系统\AltSnap-bin-x64`（本地 `make -fMakefileX64` 构建产物），测试窗口为自建 `TrayTest`（临时加入 `Processes`，验后已恢复）。

| 场景 | 结果 |
| --- | --- |
| 点 X | 窗口隐藏，`[TrayCollect] Windows=` 写入 `hwnd:pid` |
| 点托盘图标 | 窗口还原并置前，**图标保留**，`Windows=` 仍保留 |
| 再次点 X | 窗口再次隐藏，条目/图标不重复创建 |
| 窗口自行重新显示 | 条目与图标保留（不再当作新实例关闭） |
| 窗口销毁 | `Windows=` 清空，图标删除 |

仍未执行：Alt+F4（用例 4）、多窗口独立图标（3）、explorer 重启（7）、强杀还原（9）、唯一实例（13–16）。

## 待执行：spec §9 的 17 条

准备 ini（`%APPDATA%\AltSnap\AltSnap.ini` 或 exe 同目录），改完重启 AltSnap：

```ini
[TrayCollect]
Enabled=1
Processes=notepad.exe
MultiInstance=
```

| # | 用例 | 期望 | 结果 |
| --- | --- | --- | --- |
| 1 | `Processes` 为空时关任意窗口 | 行为不变（回归） | ☐ |
| 2 | 记事本点 X | 窗口消失 + 托盘多一个记事本图标；点图标还原，图标保留 | ☐ |
| 3 | 同一应用开 3 个窗口各自点 X | 3 个独立图标；Win10 上拖动其中一个其余不跟随 | ☐ |
| 4 | Alt+F4 | 同样收纳 | ☐ |
| 5 | 右键图标 | 还原 / 置顶 / 关闭 / 全部还原 全部生效 | ☐ |
| 6 | 未列入的 `mspaint.exe` 点 X | 正常关闭 | ☐ |
| 7 | 收纳后重启 `explorer.exe` | 图标自动回来 | ☐ |
| 8 | 收纳后正常退出 AltSnap | 窗口全部还原、无残留图标 | ☐ |
| 9 | 收纳后强杀 AltSnap，再启动 | 窗口自动还原，`Windows=` 被清空 | ☐ |
| 10 | 管理员权限的程序 | 记录实际表现（应为"真的关掉"，不是隐形窗口）；再用管理员 AltSnap 复测 | ☐ |
| 11 | 自定义标题栏程序 | 记录拦不到关闭的程序清单 | ☐ |
| 12 | 带 owned 对话框的程序 | 主窗口收纳、弹窗行为符合 spec §7.6 | ☐ |
| 13 | 勾选唯一实例 + 已收纳后再启动 | 已有窗口还原并置前，新窗口约 0.4s 内关掉 | ☐ |
| 14 | 取消勾选后重复 13 | 允许两个实例并行，各自能收纳 | ☐ |
| 15 | 多窗口应用（VS Code 等）勾选唯一实例 | 记录误伤情况（已知代价） | ☐ |
| 16 | 常驻托盘应用（微信/QQ）重复 13 | 记录新进程是否真的退出（任务管理器数进程） | ☐ |
| 17 | 设置页只改勾选 | Apply 按钮变亮；Apply 后 ini 两个键正确；重开页面状态一致 | ☐ |

## 已知缺口（照 spec §7.6，验收时重点观察）

1. **多窗口应用误伤**：勾选「唯一实例」时，同一 exe 的第二个真正可见窗口会被关掉，可能丢未保存内容。
2. **常驻托盘应用**：新实例收到 `WM_CLOSE` 可能只隐藏窗口、进程不退。
3. **拦不到关闭**：自定义标题栏且 `WM_NCHITTEST` 不返回 `HTCLOSE` 的程序；以及任务栏右键关闭、程序自身退出菜单。
4. **AltSnap 暂停时**（托盘菜单 Disable）：钩子被卸载，关闭拦截失效。
5. **多实例（`-m`）**：`Windows=` 键会互相覆盖。

## 需要记录到本文档的结论

- 测试环境（Windows 版本、AltSnap 是否管理员）
- 17 条结果
- 第 11、15、16 条的具体程序清单与现象
- 未修复项与建议
