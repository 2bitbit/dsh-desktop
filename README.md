# dsh-desktop

<p align="center">
  <b>DeepSeek Harness (<code>@deepseek-ai/dsh</code>) 的原生 Windows 桌面极速守护与生命周期联动 Shim</b><br>
  <a href="https://github.com/2bitbit/dsh-desktop/releases/latest"><img src="https://img.shields.io/github/v/release/2bitbit/dsh-desktop?color=blue&label=Release" alt="Release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-green.svg" alt="License"></a>
  <img src="https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20x64-blue" alt="Platform">
  <br>
  <a href="README.md.en">English Version</a>
</p>

<p align="center">
  <img src="demo.png" alt="dsh-desktop Demo" width="800">
</p>

---

## 解决的痛点

- **无控制台黑框**：纯 Win32 GUI 入口，标准 IO 绑定 `NUL`，杜绝黑色终端闪烁与 stdout 管道崩溃。
- **进程树联动销毁**：绑定 Windows Job Object，关闭 PWA 窗口瞬间秒杀所有关联 node 进程，**0 孤儿进程 / 瞬间释放 3080 端口**。
- **直通 Node.js 极速启动**：自动探测全局 `npm` / `pnpm` / `yarn` 目录直连 `node.exe`（跳过 Shell 开销；若未找到则级联回退 `pwsh` → `powershell` → `cmd`）。
- **0% CPU 挂起**：原生 `SetWinEventHook` 监听窗口销毁，配合 `MsgWaitForMultipleObjects` 内核级挂起，运行期间 **0 轮询 / 0 资源消耗**。

---

## 快速使用

### 1. 前提
- 终端已全局安装：`npm i -g @deepseek-ai/dsh` 或 `pnpm add -g @deepseek-ai/dsh`
- Edge 中已安装 PWA：访问 `http://127.0.0.1:3080/` 后点击地址栏右侧“安装应用”

### 2. 运行安装脚本
下载 [最新 Release 压缩包](https://github.com/2bitbit/dsh-desktop/releases/latest) 解压后，在 PowerShell 中执行：

```powershell
# 交互模式：直接运行，将自动弹出文件夹选择框点选工作目录
.\install_shortcut.ps1

# 参数模式：指定特定工作目录与快捷方式名称
.\install_shortcut.ps1 -TargetDirectory "D:\Workspace\MyProject"
```
> 💡 **作用说明**：生成的快捷方式**完全等效于在该目录下执行 `dsh web --no-open` 并自动唤起/接管 Edge PWA 窗口**。

---

## 架构简图

```
快捷方式 (--cwd <dir>) ──> dsh_pwa_shim.exe ──(Job Object)──> Node.js (dsh web)
                                │
                                ├─ 30ms TCP 探测 3080 端口
                                ├─ 唤起 Edge PWA 并锁定窗口 HWND/PID
                                └─ 事件挂起 (0% CPU) ──[关闭窗口]──> 秒级清理 Job 树与端口
```

---

## 常用参数

| 参数 | 说明 | 示例 |
| :--- | :--- | :--- |
| `--cwd <path>` / `-w <path>` | 指定后台 DSH 工作目录 (等效于在该目录下执行 `dsh web`) | `dsh_pwa_shim.exe --cwd "D:\Projects\Demo"` |
| `--debug` / `-d` | 启用调试日志与弹窗报错 | `dsh_pwa_shim.exe --debug` |

---

## 源码编译

```powershell
.\scripts\build.ps1
```

---

## 协议

[MIT License](LICENSE)
