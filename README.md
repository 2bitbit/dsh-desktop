# dsh-desktop

<p align="center">
  <b>DeepSeek Harness (<code>@deepseek-ai/dsh</code>) 的原生 Windows 桌面启动器</b><br>
  点击快捷方式 = 在指定目录执行 <code>dsh web --no-open</code>，解析本次进程 token，最大化打开 Edge 应用窗；窗口收进托盘常驻，托盘菜单退出时才回收后台。
  <br><br>
  <a href="https://github.com/2bitbit/dsh-desktop/releases/latest"><img src="https://img.shields.io/github/v/release/2bitbit/dsh-desktop?color=blue&label=Release" alt="Release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-green.svg" alt="License"></a>
  <img src="https://img.shields.io/badge/Platform-Windows%2010%201607%2B%20%2F%2011%20x64-blue" alt="Platform">
  <img src="https://img.shields.io/badge/Requires-dsh%20%3E%3D%200.1.2-informational" alt="Requires dsh">
  <br>
  <a href="README.md.en">English</a>
</p>

<p align="center">
  <img src="demo.png" alt="dsh-desktop Demo" width="800">
</p>

> [!IMPORTANT]
> **仅限 Windows 10 1607+ / 11 x64**。依赖 Win32 GUI、Job Object、WinEventHook、低层输入钩子、每显示器 DPI 感知 v2，以及本机已安装的 Microsoft Edge。
>
> **不需要**在 Edge 里预先安装任何应用：本程序用 `msedge.exe --app=<tokenUrl>` 直接开独立窗口。

---

## 做什么

`dsh web` 从 0.1.2 起在根路径强制认证：每次启动都会打印带 `?token=` 的一次性 URL，浏览器用它换 cookie 后再进界面。无 token 直接打开 `http://127.0.0.1:3080/` 会 401。

本仓库把这条链路收成一个无控制台 exe：

1. 单实例检查：已有实例在跑时，只把它的窗口前置，然后自己退出。
2. 立刻弹出原生启动窗（可取消）。
3. 直通 pnpm 的 `node.exe` 跑 `dsh web --no-open`，stdout 进管道。
4. 解析 `dsh web: http://127.0.0.1:<port>/?token=...`。
5. 只打开一次 Edge `--app=<tokenUrl>`，并最大化；随后挂出托盘图标。
6. 窗口关掉（叉号 / Alt+F4 / Ctrl+Shift+W）只是**收进托盘**，后台继续跑。
7. 托盘菜单「退出」→ Job Object 秒杀 node 进程树，端口立刻释放。

---

## 关闭语义

| 操作 | 结果 |
| :--- | :--- |
| 左键点标题栏叉号 | 收进托盘，后台继续运行 |
| `Alt+F4`（应用窗口在前台） | 同上 |
| `Ctrl+Shift+W`（应用窗口在前台） | 同上 |
| 左键单击托盘图标 / 再次运行 exe | 前置已有窗口 |
| 右键托盘图标 → 显示窗口 | 同上 |
| 右键托盘图标 → 退出并结束后台 | 关窗口 + 杀后台进程树 + 移除托盘图标 + 退出 |
| 窗口被其它途径销毁（任务栏预览关闭、浏览器崩溃等） | 同样回收后台再退出，不留僵尸进程 |

三条「收进托盘」的路径都不弹任何通知，也不会让后台进程退出。

热键只在**应用窗口处于前台**时才拦，不会抢其它程序的 `Alt+F4` / `Ctrl+Shift+W`。

---

## 快速使用

### 1. 前提

- Windows 10 1607+ / 11 x64，已安装 Microsoft Edge
- 已全局安装 dsh **≥ 0.1.2**：`pnpm add -g @deepseek-ai/dsh`

### 2. 安装快捷方式

下载 [最新 Release 压缩包](https://github.com/2bitbit/dsh-desktop/releases/latest) 解压后，在 PowerShell 中执行：

```powershell
# 交互模式：弹出文件夹选择框
.\install_shortcut.ps1

# 指定工作目录与快捷方式名称
.\install_shortcut.ps1 -TargetDirectory "D:\Workspace\MyProject"
```

会在**桌面与开始菜单**各放一个快捷方式。点击它 = 在该目录执行 `dsh web --no-open`，等 token 就绪后打开最大化的 Edge 应用窗。

托盘图标取自 exe 内嵌资源（`src/tray_icon.ico`），快捷方式图标同样用的是它。

---

## 生命周期

```
快捷方式 (--cwd <dir>)
        │
        ▼
dsh_desktop.exe ── 单实例检查（已有实例 → 只前置窗口，然后本进程退出）
        │
        ├─ 启动窗
        │
        ├─ Job Object ──▶ node (dsh web --no-open)
        │                     │
        │                     └─ stdout: dsh web: http://127.0.0.1:<port>/?token=***
        │
        ├─ Edge --app=<tokenUrl>  （最大化）
        │
        ├─ 托盘图标常驻
        │
        ├─ 叉号 / Alt+F4 / Ctrl+Shift+W ──▶ 隐藏到托盘（后台照常跑）
        │
        └─ 托盘菜单「退出」 ──▶ TerminateJobObject ──▶ 回收进程树 / 释放端口
```

运行期间用 `SetWinEventHook` + `MsgWaitForMultipleObjects` 挂起，**0 轮询 / 0% CPU**。

叉号之所以能被拦下：程序挂了一个 `WH_MOUSE_LL` 低层鼠标钩子，左键按下时若点在窗口顶部带内，就向应用窗口问一次 `WM_NCHITTEST`；Chromium 会为自绘标题栏如实回答，关闭按钮返回 `HTCLOSE`。只有拿到 `HTCLOSE` 才吞掉这次点击并隐藏窗口，页面内的普通点击不受影响。

---

## 常用参数

| 参数 | 说明 | 示例 |
| :--- | :--- | :--- |
| `--cwd <path>` / `-w <path>` | 后台 dsh 工作目录 | `dsh_desktop.exe --cwd "D:\Projects\Demo"` |
| `--debug` / `-d` | 调试日志 + 失败弹窗 | `dsh_desktop.exe --debug` |

exe 同级会写两份日志（每次启动清空，运行中追加并限大小；token 脱敏）：

| 文件 | 内容 | 上限 |
| :--- | :--- | :--- |
| `dsh_desktop.log` | 启动器自身日志 | 512 KB |
| `dsh_web_console.log` | `dsh web` 的 stdout/stderr | 2 MB |

---

## 源码编译

需要 MinGW-w64 / LLVM Clang / MSVC 之一，以及配套的资源编译器（`windres` 或 `rc.exe`）：

```powershell
.\scripts\build.ps1
```

脚本会先把 `src/dsh_desktop.rc`（内嵌托盘图标 `src/tray_icon.ico`）编译成资源对象，再连同 `src/dsh_desktop.cpp` 一起链接。

产物：`bin/dsh_desktop.exe`。GCC 路径用 `-static -static-libgcc -static-libstdc++` 静态链接，成品不依赖 MinGW 运行时 DLL。

> GCC 路径已在 MinGW-w64 GCC 15 上实测；Clang / MSVC 两条路径未实测。

---

## 版本沿革

见 [CHANGELOG.md](CHANGELOG.md)。

---

## 协议

[MIT License](LICENSE)
