# Changelog

## v2.0.0

适配 `@deepseek-ai/dsh` ≥ 0.1.2 的根路径强制认证。关闭窗口仍会回收整个 node 进程树。

### 破坏性变更

- 不再使用预先安装的 Edge PWA / `--app-id`。改为 `msedge.exe --app=<tokenUrl>`。
- 不再探活固定端口 `127.0.0.1:3080`。改为解析本次进程 stdout 中的 `dsh web: http://127.0.0.1:<port>/?token=...`。
- 直通启动只认 pnpm 全局 `dsh` shim（`cmd-shim-target`），不再级联 `pwsh` / `cmd`。

### 新增

- 原生启动窗：token 未就绪前不打开浏览器，避免 401。
- 捕获到 Edge 应用窗后自动最大化。
- `dsh web` 的 stdout/stderr 写入 exe 同级 `dsh_web_console.log`（2 MB 防爆）。
- shim 日志每次启动清空；token 写入日志时脱敏。

## v1.0.0

初始版本：探测 3080、唤起已安装的 Edge PWA、Job Object 生命周期联动。
