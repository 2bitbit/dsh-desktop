# dsh-desktop (DSH PWA Shim)

<p align="center">
  <b>为 DeepSeek Harness (<code>@deepseek-ai/dsh</code>) 打造的极速、零控制台黑框、纯事件驱动的 Windows 桌面级生命周期守护程序。</b>
</p>

<p align="center">
  <a href="https://github.com/2bitbit/dsh-desktop/releases/latest"><img src="https://img.shields.io/github/v/release/2bitbit/dsh-desktop?color=blue&label=Release" alt="Release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-green.svg" alt="License"></a>
  <img src="https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20x64-blue" alt="Platform">
  <br>
  <a href="README.md.en">English Version</a>
</p>

---

## 📖 目录

- [为什么需要 dsh-desktop？](#为什么需要-dsh-desktop)
- [核心特性](#核心特性)
- [架构与生命周期](#架构与生命周期)
- [快速使用](#快速使用)
- [包管理器与环境兼容](#包管理器与环境兼容)
- [源码构建](#源码构建)
- [命令行参数说明](#命令行参数说明)
- [开源协议](#-开源协议)

---

## 为什么需要 dsh-desktop？

在使用 `dsh web` 时，Windows 开发者常遇到以下痛点：
1. **控制台黑框干扰**：通过 PowerShell 或 CMD 启动会伴随持续驻留的黑色终端窗口。
2. **孤儿进程与端口泄漏**：手动关闭浏览器标签页或 PWA 后，后台的 `node.exe` 进程无法联动退出，导致再次启动时 `3080` 端口被占。
3. **冷启动迟滞**：每次拉起 Shell 都需要加载 Profile 和环境脚本，耗费额外时间。

`dsh-desktop` 通过一个 **~107KB 的原生 C++ Win32 守护 Shim**，彻底实现了桌面应用级的平滑体验。

---

## 核心特性

- ⚡ **直通 Node.js 极速启动**：自动探测全局 `npm` / `pnpm` / `yarn` 目录下的 `@deepseek-ai/dsh` 入口，直连 `node.exe` 毫秒级拉起。
- 🪟 **零黑框静默运行**：Win32 GUI 原生入口（`WinMain`），并将标准输入输出绑定到 `NUL` 设备，杜绝无控制台模式下的 stdout 管道崩溃。
- 🛡️ **严格内核级 Job Object 托管**：子进程严格加入 Windows 作业对象，配置 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 并禁止 Breakaway 逃逸。
- ⏱️ **30ms 高频健康探测与窗口捕获**：30ms 极速轮询 `127.0.0.1:3080` 监听状态与 Edge PWA 窗口句柄（HWND/PID）。
- 💤 **纯事件驱动零占用等待**：通过 `SetWinEventHook` 监听 `EVENT_OBJECT_DESTROY`，配合 `MsgWaitForMultipleObjects` 内核级挂起，运行期间 **0% CPU / 0 轮询**。
- 🧹 **秒级级联回收**：PWA 窗口关闭瞬间自动触发 `TerminateJobObject`，瞬间终结全部关联进程，彻底释放 3080 端口。
- 📁 **多工作区快捷方式隔离**：支持通过 `--cwd <路径>` 传入不同工作目录，轻松为不同代码库定制独立快捷方式。
- 🛡️ **磁盘防爆日志**：自动在同级目录维护 `dsh_pwa_shim.log`，内置 512KB 上限自动覆写机制。

---

## 架构与生命周期

```
[ 快捷方式 / Launcher ] --cwd "D:\MyProject"
         │
         ▼
 ┌───────────────────────────────────────────────────────────┐
 │                   dsh_pwa_shim.exe                        │
 │  1. 创建严格隔离的 Windows Job Object                     │
 │  2. 直通 Node.js (@deepseek-ai/dsh) 注入 Job 对象         │
 │  3. 30ms 非阻塞快速探测 127.0.0.1:3080 服务端口          │
 │  4. 唤起 Microsoft Edge PWA 独立窗口                     │
 │  5. 30ms 极速锁定 PWA HWND 与宿主 PID                     │
 │  6. 注册 SetWinEventHook (EVENT_OBJECT_DESTROY)           │
 │  7. MsgWaitForMultipleObjects 内核挂起 (0% CPU / 0 轮询)  │
 └─────────────────────────────┬─────────────────────────────┘
                               │ (用户关闭 PWA 窗口)
                               ▼
 ┌───────────────────────────────────────────────────────────┐
 │                   瞬间级联回收                            │
 │  - WinEventHook 捕获窗口销毁事件                          │
 │  - TerminateJobObject 强制秒杀所有关联 node 进程          │
 │  - 3080 端口瞬间释放，Shim 干净退出                       │
 └───────────────────────────────────────────────────────────┘
```

---

## 快速使用

### 前置要求
1. **安装 DSH**：终端执行 `npm i -g @deepseek-ai/dsh` 或 `pnpm add -g @deepseek-ai/dsh`。
2. **在 Edge 中安装 PWA**：启动一次 DSH，浏览器访问 `http://127.0.0.1:3080/` 后，点击 Edge 地址栏右侧的“安装应用”。

### 方式 A：直接下载预编译发布包（推荐）
1. 前往 [Releases](https://github.com/2bitbit/dsh-desktop/releases/latest) 下载 `dsh-desktop-v1.0.0-windows-x64.zip`。
2. 解压至任意目录（如 `D:\Tools\dsh-desktop`）。
3. 在该目录下打开 PowerShell 执行一键创建快捷方式：
   ```powershell
   .\install_shortcut.ps1 -TargetDirectory "D:\你的工作区目录"
   ```

### 方式 B：从源码构建
```powershell
git clone https://github.com/2bitbit/dsh-desktop.git
cd dsh-desktop
.\scripts\build.ps1
.\scripts\install_shortcut.ps1 -TargetDirectory "D:\你的工作区目录"
```

---

## 包管理器与环境兼容

`dsh-desktop` 内置全自动包定位与多级 Shell 级联回退：

| 阶段 | 机制 / 目标 | 适用场景 |
| :--- | :--- | :--- |
| **直通 Phase 1** | `npm` 全局目录 (`%APPDATA%\npm\node_modules`) | 使用 `npm i -g` 安装 |
| **直通 Phase 2** | `pnpm` 全局版本目录 (`%LOCALAPPDATA%\pnpm\global\*`) | 使用 `pnpm add -g` 安装 |
| **直通 Phase 3** | `yarn` 全局目录 (`%LOCALAPPDATA%\Yarn\Data\global`) | 使用 `yarn global add` 安装 |
| **级联回退 1** | `pwsh.exe` (PowerShell 7) | 安装了 PowerShell 7 的环境 |
| **级联回退 2** | `powershell.exe` (Windows PowerShell 5.1) | 所有 Win10/Win11 系统标配 |
| **级联回退 3** | `cmd.exe` (Command Prompt) | 终极兜底方案 |

---

## 源码构建

支持系统任意现代 C++17 编译器（MinGW-w64 `g++`、LLVM `clang++` 或 Visual Studio `cl.exe`）。

```powershell
# 自动探测编译器并生成极致优化体积的二进制 (bin/dsh_pwa_shim.exe)
.\scripts\build.ps1

# 手动指定编译器
.\scripts\build.ps1 -Compiler GCC
.\scripts\build.ps1 -Compiler Clang
.\scripts\build.ps1 -Compiler MSVC

# 生成调试版
.\scripts\build.ps1 -DebugBuild
```

---

## 命令行参数说明

| 参数 | 说明 | 示例 |
| :--- | :--- | :--- |
| `--cwd <path>` / `-w <path>` | 指定后台 dsh 运行时所在工作目录 | `dsh_pwa_shim.exe --cwd "D:\Workspace\MyProject"` |
| `--debug` / `-d` | 启用调试模式日志与 Win32 弹窗报错 | `dsh_pwa_shim.exe --debug` |

---

## 📄 开源协议

本项目基于 [MIT License](LICENSE) 开源。
