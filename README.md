# dsh-desktop (DSH PWA Shim)

<p align="center">
  <b>A lightweight, zero-overhead native Windows lifecycle shim & launcher for DeepSeek Harness (<code>@deepseek-ai/dsh</code>) PWA.</b><br>
  为 <b>DeepSeek Harness</b> 打造的极速、零控制台黑框、纯事件驱动的 Windows 桌面级生命周期守护程序。
</p>

<p align="center">
  <a href="https://github.com/2bitbit/dsh-desktop/releases/latest"><img src="https://img.shields.io/github/v/release/2bitbit/dsh-desktop?color=blue&label=Release" alt="Release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-green.svg" alt="License"></a>
  <img src="https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20x64-blue" alt="Platform">
</p>

---

## 📖 目录 / Table of Contents

- [English](#-english)
  - [Why dsh-desktop?](#why-dsh-desktop)
  - [Key Features](#key-features)
  - [Architecture & Lifecycle](#architecture--lifecycle)
  - [Quick Start](#quick-start)
  - [Building from Source](#building-from-source)
  - [CLI Arguments](#cli-arguments)
- [简体中文](#-简体中文)
  - [为什么需要 dsh-desktop？](#为什么需要-dsh-desktop)
  - [核心特性](#核心特性)
  - [架构与生命周期](#架构与生命周期)
  - [快速使用](#快速使用)
  - [源码构建](#源码构建)
  - [命令行参数说明](#命令行参数说明)

---

# 🌐 English

### Why dsh-desktop?

When using `dsh web`, Windows developers usually encounter several pain points:
1. **Console Window Clutter**: Launching via `pwsh` or `cmd` generates an annoying terminal window.
2. **Zombie Process & Port Leak**: Closing the browser window does not terminate the background `node.exe` processes, locking port `3080`.
3. **Slow Cold Startup**: Shell initialization profiles add hundreds of milliseconds of latency.

`dsh-desktop` solves all of these issues by providing a **~98KB native C++ Win32 GUI shim** that manages the entire lifecycle seamlessly.

### Key Features

- ⚡ **Direct Node.js Bypass**: Automatically locates `@deepseek-ai/dsh` in the global `pnpm` store, launching directly via `node.exe` with zero PowerShell/CMD startup overhead (graceful fallback to `pwsh` if needed).
- 🪟 **Zero Console Black Windows**: Native Win32 `WinMain` entrypoint with explicit `NUL` device redirection to prevent stdout/stderr pipe crashes in windowless mode.
- 🛡️ **Strict Job Object Supervision**: Configured with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` and breakaway prevention. When the shim closes, all child node processes are killed instantly.
- ⏱️ **30ms Fast Probing**: 30ms non-blocking socket check for `127.0.0.1:3080` readiness and fast PWA window HWND capture.
- 💤 **Pure Event-Driven (0% CPU)**: Leverages `SetWinEventHook` (`EVENT_OBJECT_DESTROY`) + `MsgWaitForMultipleObjects` to sleep until the PWA window is closed. Zero polling, zero CPU usage.
- 📁 **Per-Project Working Directory**: Accepts `--cwd <path>` / `-w <path>` via shortcut arguments to open specific project workspaces.
- 🛡️ **Log Rotation Guard**: Self-contained `dsh_pwa_shim.log` capped at 512 KB to avoid disk bloating.

---

### Architecture & Lifecycle

```
[ Shortcut / Launcher ] --cwd "D:\MyProject"
         │
         ▼
 ┌───────────────────────────────────────────────────────────┐
 │                   dsh_pwa_shim.exe                        │
 │  1. Create isolated Windows Job Object                    │
 │  2. Launch Node.js (@deepseek-ai/dsh) directly into Job   │
 │  3. 30ms non-blocking probe on 127.0.0.1:3080             │
 │  4. Launch Microsoft Edge PWA client                      │
 │  5. Capture PWA HWND & PID (30ms interval)                │
 │  6. SetWinEventHook on EVENT_OBJECT_DESTROY               │
 │  7. MsgWaitForMultipleObjects (Sleep / 0% CPU)            │
 └─────────────────────────────┬─────────────────────────────┘
                               │ (User Closes PWA Window)
                               ▼
 ┌───────────────────────────────────────────────────────────┐
 │                   Instant Teardown                        │
 │  - WinEventHook triggered                                 │
 │  - TerminateJobObject kills all node processes instantly   │
 │  - Port 3080 released immediately                         │
 └───────────────────────────────────────────────────────────┘
```

---

### Quick Start

#### Prerequisites
1. **Node.js & DSH Installed**: Run `pnpm add -g @deepseek-ai/dsh` in terminal.
2. **Edge PWA Installed**: Start dsh once, open `http://127.0.0.1:3080/` in Microsoft Edge, and click the "Install App" button in the address bar.

#### Option A: Download Pre-built Release (Recommended)
1. Download `dsh-desktop-v1.0.0-windows-x64.zip` from [Releases](https://github.com/2bitbit/dsh-desktop/releases/latest).
2. Extract to any directory (e.g. `D:\Tools\dsh-desktop`).
3. Run in PowerShell to create shortcuts:
   ```powershell
   .\install_shortcut.ps1 -TargetDirectory "D:\Your\Workspace\Directory"
   ```

#### Option B: Build from Source
```powershell
git clone https://github.com/2bitbit/dsh-desktop.git
cd dsh-desktop
.\scripts\build.ps1
.\scripts\install_shortcut.ps1 -TargetDirectory "D:\Your\Workspace\Directory"
```

---

### Building from Source

Requirements: Any modern C++17 compiler (`g++` / MinGW-w64, `clang++`, or MSVC `cl.exe`).

```powershell
# Auto-detect compiler and build optimized release binary
.\scripts\build.ps1

# Force GCC
.\scripts\build.ps1 -Compiler GCC

# Build with debug symbols
.\scripts\build.ps1 -DebugBuild
```

---

### CLI Arguments

| Argument | Description | Example |
| :--- | :--- | :--- |
| `--cwd <path>` / `-w <path>` | Set working directory for the background DSH server | `dsh_pwa_shim.exe --cwd "D:\Projects\Demo"` |
| `--debug` / `-d` | Enable debug logging and error popup dialogs | `dsh_pwa_shim.exe --debug` |

---

# 🇨🇳 简体中文

### 为什么需要 dsh-desktop？

在使用 `dsh web` 时，Windows 开发者常遇到以下痛点：
1. **控制台黑框干扰**：通过 PowerShell 或 CMD 启动会伴随持续驻留的黑色终端窗口。
2. **孤儿进程与端口泄漏**：手动关闭浏览器标签页或 PWA 后，后台的 `node.exe` 进程无法联动退出，导致再次启动时 `3080` 端口被占。
3. **冷启动迟滞**：每次拉起 Shell 都需要加载 Profile 和环境脚本，耗费额外时间。

`dsh-desktop` 通过一个 **~98KB 的原生 C++ Win32 守护 Shim**，彻底实现了桌面应用级的平滑体验。

---

### 核心特性

- ⚡ **直通 Node.js 极速启动**：自动探测全局 `pnpm` 目录下的 `@deepseek-ai/dsh` 入口，直连 `node.exe` 毫秒级拉起（若探测失败则自动安全降级至 `pwsh`）。
- 🪟 **零黑框静默运行**：Win32 GUI 原生入口（`WinMain`），并将标准输入输出绑定到 `NUL` 设备，杜绝无控制台模式下的 stdout 管道崩溃。
- 🛡️ **严格内核级 Job Object 托管**：子进程严格加入 Windows 作业对象，配置 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 并禁止 Breakaway 逃逸。
- ⏱️ **30ms 高频健康探测与窗口捕获**：30ms 极速轮询 `127.0.0.1:3080` 监听状态与 Edge PWA 窗口句柄（HWND/PID）。
- 💤 **纯事件驱动零占用等待**：通过 `SetWinEventHook` 监听 `EVENT_OBJECT_DESTROY`，配合 `MsgWaitForMultipleObjects` 内核级挂起，运行期间 **0% CPU / 0 轮询**。
- 🧹 **秒级级联回收**：PWA 窗口关闭瞬间自动触发 `TerminateJobObject`，瞬间终结全部关联进程，彻底释放 3080 端口。
- 📁 **多工作区快捷方式隔离**：支持通过 `--cwd <路径>` 传入不同工作目录，轻松为不同代码库定制独立快捷方式。
- 🛡️ **磁盘防爆日志**：自动在同级目录维护 `dsh_pwa_shim.log`，内置 512KB 上限自动覆写机制。

---

### 快速使用

#### 前置要求
1. **全局安装 DSH**：终端执行 `pnpm add -g @deepseek-ai/dsh`。
2. **Edge 中安装 PWA**：启动一次 DSH，浏览器访问 `http://127.0.0.1:3080/` 后，点击 Edge 地址栏右侧的“安装应用”。

#### 方式 A：直接下载预编译发布包（推荐）
1. 前往 [Releases](https://github.com/2bitbit/dsh-desktop/releases/latest) 下载 `dsh-desktop-v1.0.0-windows-x64.zip`。
2. 解压至任意目录（如 `D:\Tools\dsh-desktop`）。
3. 在该目录下打开 PowerShell 创建快捷方式：
   ```powershell
   .\install_shortcut.ps1 -TargetDirectory "D:\你的工作区目录"
   ```

#### 方式 B：从源码编译
```powershell
git clone https://github.com/2bitbit/dsh-desktop.git
cd dsh-desktop
.\scripts\build.ps1
.\scripts\install_shortcut.ps1 -TargetDirectory "D:\你的工作区目录"
```

---

### 源码构建

支持系统任意现代 C++17 编译器（MinGW-w64 `g++`、LLVM `clang++` 或 Visual Studio `cl.exe`）。

```powershell
# 自动探测编译器并生成体积优化可执行文件 (bin/dsh_pwa_shim.exe)
.\scripts\build.ps1

# 手动指定编译器
.\scripts\build.ps1 -Compiler GCC
.\scripts\build.ps1 -Compiler Clang
.\scripts\build.ps1 -Compiler MSVC

# 生成调试版
.\scripts\build.ps1 -DebugBuild
```

---

### 命令行参数说明

| 参数 | 说明 | 示例 |
| :--- | :--- | :--- |
| `--cwd <path>` / `-w <path>` | 指定后台 dsh 运行时所在工作目录 | `dsh_pwa_shim.exe --cwd "D:\Workspace\MyProject"` |
| `--debug` / `-d` | 启用调试模式日志与 Win32 弹窗报错 | `dsh_pwa_shim.exe --debug` |

---

## 📄 开源协议 / License

本项目基于 [MIT License](LICENSE) 开源。
