# Changelog

## v2.2.0

把 PWA 这个名字从项目里彻底去掉：`dsh_pwa_shim.*` → **`dsh_desktop.*`**。这是一次**破坏性改名**，且不提供任何兼容层。

### 破坏性变更

- 可执行文件 `dsh_pwa_shim.exe` → **`dsh_desktop.exe`**；源码与资源脚本同步改为 `src/dsh_desktop.cpp` / `src/dsh_desktop.rc`。
- 日志文件 `dsh_pwa_shim.log` → **`dsh_desktop.log`**（`dsh_web_console.log` 不变）。
- 窗口类名 `DshPwaShimSplash` / `DshPwaShimController` → `DshDesktopSplash` / `DshDesktopController`。
- `scripts/build.ps1`、`scripts/install_shortcut.ps1`、两份 README、Release 资产名一并改用新名字。

### 升级方式

旧快捷方式指向的 `dsh_pwa_shim.exe` 已不存在，因此：

1. 从托盘菜单退出正在运行的旧实例；
2. 用新版解压目录里的 `install_shortcut.ps1` 重新创建快捷方式（或手动把快捷方式指向 `dsh_desktop.exe`）；
3. 删除旧快捷方式与旧的 `dsh_pwa_shim.log`。

本版**没有**兼容层：不保留旧文件名副本，不做旧日志路径、旧窗口类名、旧实例的回退识别。单实例锚点随类名一起变化，新旧两版互不识别——这是刻意的。

### 说明

- 命名依据：本版用 `msedge.exe --app=<url>` 打开的是 Chromium 的 **app 模式窗口**，与「在 Edge 里预先安装的应用 / `--app-id`」无关。v2.0.0 起行为已经如此，v2.2.0 只是把名字改到与行为一致。
- 功能与 v2.1.0 完全相同：托盘常驻、单实例、叉号 / Alt+F4 / Ctrl+Shift+W 收进托盘、托盘菜单真关闭。

## v2.1.0

托盘常驻 + 单实例 + 关闭语义重做：叉号不再是「退出」，而是收进托盘。

### 行为变更（注意）

- **左键点标题栏叉号 = 收进托盘**，不再关窗、不再回收后台。窗口本身不会被销毁，后台 node 进程继续跑。
- **`Alt+F4`、`Ctrl+Shift+W` 同样收进托盘**（仅当应用窗口处于前台时才拦，不抢其它程序的快捷键）。
- **真正的关闭改为右键托盘图标 → 「退出并结束后台」**：关窗口 + `TerminateJobObject` 回收进程树 + 移除托盘图标。
- 窗口若被其它途径销毁（任务栏预览的关闭按钮、浏览器崩溃等），仍然回收后台再退出，不留僵尸进程。
- 三条「收进托盘」路径都不弹任何通知。

### 新增

- **托盘图标常驻**：左键单击前置窗口，右键弹菜单（显示窗口 / 退出并结束后台）。
- **单实例**：隐藏控制窗 + `FindWindow` 判定。重复运行 exe 只把已有窗口前置（并把前台特权转交过去），不再起第二套后台。
- **托盘图标内嵌进 exe**（`src/tray_icon.ico`，多尺寸 16/20/24/32/40/48/64/256）。快捷方式图标同步改为取 exe 内嵌图标。
- 资源脚本 `src/dsh_pwa_shim.rc`；`scripts/build.ps1` 增加资源编译步骤，缺资源编译器直接报错。
- 进程按**每显示器 DPI 感知 v2** 运行——低层钩子给的是物理坐标，不感知 DPI 会让关闭按钮判定差一个缩放比。

### 变更

- 构建改为**静态链接**（`-static -static-libgcc -static-libstdc++`），成品不再依赖 MinGW 运行时 DLL；exe 体积由约 200 KB 变为约 500 KB（strip 后）。
- 术语统一：受管窗口在代码里叫「应用窗口 / App」（`APP_WINDOW_CLASS`、`g_AppClosed` 等）。

### 要求的平台

Windows 10 **1607+** / 11 x64（DPI 感知 v2 与低层输入钩子的下限）。

## v2.0.0

适配 `@deepseek-ai/dsh` ≥ 0.1.2 的根路径强制认证。关闭窗口仍会回收整个 node 进程树。

### 破坏性变更

- 不再使用预先安装的 Edge 应用（`--app-id`）。改为 `msedge.exe --app=<tokenUrl>` 直接开独立窗口。
- 不再探活固定端口 `127.0.0.1:3080`。改为解析本次进程 stdout 中的 `dsh web: http://127.0.0.1:<port>/?token=...`。
- 直通启动只认 pnpm 全局 `dsh` shim（`cmd-shim-target`），不再级联 `pwsh` / `cmd`。

### 新增

- 原生启动窗：token 未就绪前不打开浏览器，避免 401。
- 捕获到 Edge 应用窗后自动最大化。
- `dsh web` 的 stdout/stderr 写入 exe 同级 `dsh_web_console.log`（2 MB 防爆）。
- 日志每次启动清空；token 写入日志时脱敏。

## v1.0.0

初始版本：探测 3080、唤起预先安装的 Edge 应用、Job Object 生命周期联动。
