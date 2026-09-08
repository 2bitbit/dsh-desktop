# dsh-desktop

<p align="center">
  <b>DeepSeek Harness (<code>@deepseek-ai/dsh</code>) 的原生 Windows 桌面启动器</b><br>
  点击快捷方式 = 在指定目录执行 <code>dsh web --no-open</code>，解析本次进程 token，最大化打开 Edge 应用窗；关窗即回收后台。
  <br><br>
  <a href="https://github.com/2bitbit/dsh-desktop/releases/latest"><img src="https://img.shields.io/github/v/release/2bitbit/dsh-desktop?color=blue&label=Release" alt="Release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-green.svg" alt="License"></a>
  <img src="https://img.shields.io/badge/Platform-Windows%2010%2F11%20x64-blue" alt="Platform">
  <img src="https://img.shields.io/badge/Requires-dsh%20%3E%3D%200.1.2-informational" alt="Requires dsh">
  <br>
  <a href="README.md.en">English</a>
</p>

<p align="center">
  <img src="demo.png" alt="dsh-desktop Demo" width="800">
</p>

> [!IMPORTANT]
> **仅限 Windows 10 / 11 x64**。依赖 Win32 GUI、Job Object、WinEventHook，以及本机已安装的 Microsoft Edge。
>
> **不需要**先在 Edge 里「安装应用 / PWA」。v2 用 `msedge.exe --app=<tokenUrl>` 直接开独立窗口。

---

## 做什么

`dsh web` 从 0.1.2 起在根路径强制认证：每次启动都会打印带 `?token=` 的一次性 URL，浏览器用它换 cookie 后再进界面。无 token 直接打开 `http://127.0.0.1:3080/` 会 401。

本仓库把这条链路收成一个无控制台 exe：

1. 立刻弹出原生启动窗（可取消）。
2. 直通 pnpm 的 `node.exe` 跑 `dsh web --no-open`，stdout 进管道。
3. 解析 `dsh web: http://127.0.0.1:<port>/?token=...`。
4. 只打开一次 Edge `--app=<tokenUrl>`，并最大化。
5. 窗口关掉 → Job Object 秒杀 node 进程树，端口立刻释放。

---

## 快速使用

### 1. 前提

- Windows 10 / 11 x64，已安装 Microsoft Edge
- 已全局安装 dsh **≥ 0.1.2**：`pnpm add -g @deepseek-ai/dsh`

无需安装 Edge PWA。

### 2. 安装快捷方式

下载 [最新 Release 压缩包](https://github.com/2bitbit/dsh-desktop/releases/latest) 解压后，在 PowerShell 中执行：

```powershell
# 交互模式：弹出文件夹选择框
.\install_shortcut.ps1

# 指定工作目录与快捷方式名称
.\install_shortcut.ps1 -TargetDirectory "D:\Workspace\MyProject"
```

会在**桌面与开始菜单**各放一个快捷方式。点击它 = 在该目录执行 `dsh web --no-open`，等 token 就绪后打开最大化的 Edge 应用窗。

---

## 生命周期

```
快捷方式 (--cwd <dir>)
        │
        ▼
dsh_pwa_shim.exe ── 启动窗
        │
        ├─ Job Object ──▶ node (dsh web --no-open)
        │                     │
        │                     └─ stdout: dsh web: http://127.0.0.1:<port>/?token=***
        │
        ├─ Edge --app=<tokenUrl>  （最大化）
        │
        └─ 关窗 ──▶ TerminateJobObject ──▶ 回收进程树 / 释放端口
```

运行期间用 `SetWinEventHook` + `MsgWaitForMultipleObjects` 挂起，**0 轮询 / 0% CPU**。

---

## 常用参数

| 参数 | 说明 | 示例 |
| :--- | :--- | :--- |
| `--cwd <path>` / `-w <path>` | 后台 dsh 工作目录 | `dsh_pwa_shim.exe --cwd "D:\Projects\Demo"` |
| `--debug` / `-d` | 调试日志 + 失败弹窗 | `dsh_pwa_shim.exe --debug` |

exe 同级会写两份日志（每次启动清空，运行中追加并限大小；token 脱敏）：

| 文件 | 内容 | 上限 |
| :--- | :--- | :--- |
| `dsh_pwa_shim.log` | shim 自身日志 | 512 KB |
| `dsh_web_console.log` | `dsh web` 的 stdout/stderr | 2 MB |

---

## 相对 v1.0 的变化

- **不再依赖预先安装的 Edge PWA**，也不再硬编码 `--app-id`。
- **不再探活 3080 端口**。改为解析本次进程打印的认证 URL（端口可变）。
- 原生启动窗：token 未就绪前不打开浏览器，避免 401。
- 捕获窗口后自动最大化。
- `dsh web` 控制台输出落到 `dsh_web_console.log`。

---

## 源码编译

需要 MinGW-w64 / LLVM Clang / MSVC 之一：

```powershell
.\scripts\build.ps1
```

产物：`bin/dsh_pwa_shim.exe`。

---

## 协议

[MIT License](LICENSE)
