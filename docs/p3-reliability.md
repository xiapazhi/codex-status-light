<!--
文件作用：记录 Codex Status Light P3 可靠性阶段的实现和验证结果
职责范围：
1. 说明 Unknown、Stale、文件截断、监听恢复和进程联动的实现边界
2. 记录 P3 阶段的本机验证结果
3. 为后续 P4 发布验收提供阶段依据

不负责：
- 重新整理 P0 事件映射证据
- 保存用户提示词、模型回复正文或工具输出正文

维护说明：
- 本文只记录阶段实现事实；事件语义仍以 docs/codex-event-map.md 为准
-->

# P3 Reliability

Delivered on 2026-08-05.

## Implemented

```text
Unknown / Stale:
- Explicit JSONL events still take priority over time-based inference.
- Only a Running session with no event for staleMinutes, default 30 minutes, becomes Stale.
- Unknown and Stale both render as gray center plus orange warning badge.
- Tooltip and diagnostics separate Unknown from Stale.

File truncation:
- Each rollout file keeps offset, knownSize, incompleteLine, and consecutiveErrors.
- If file size becomes smaller than offset, the file is treated as truncated or replaced.
- Sessions previously contributed by that file are removed before rereading it from the beginning.
- A final line without newline is stored and parsed only after the next read completes it.

Watcher recovery:
- Tray mode uses ReadDirectoryChangesW recursively on the sessions directory.
- The watcher thread only posts a main-thread signal and never touches tray UI directly.
- A 5 second calibration scan remains active and retries watcher setup after failure.

Process lifecycle:
- CreateToolhelp32Snapshot / Process32FirstW / Process32NextW are used for Codex process counts.
- Process presence is used only for exit policy and diagnostics, not for marking tasks Running.
- Startup waits up to 15 seconds for Codex.
- During runtime, no Codex process and no user-visible task exits after 5 seconds.

Aggregation:
- Priority is WaitingInput > Failed > Running > Completed > Stale > Unknown > Idle.
- Completed acknowledgement remains in current-process memory only.

Single instance:
- Tray mode uses Local\CodexStatusLightSingleInstance to avoid duplicate tray processes.
```

## Validation

```text
Release x64 build: 0 warnings, 0 errors
StatusLight.exe --status: parsed 4166 lines, JSON errors 0, running 1, completed 3, cancelled 1, stale 0
Tray short run: process stayed alive for 4 seconds and was stopped after validation
Single-instance check: first tray process alive, second tray process exited
```
