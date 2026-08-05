<!--
文件作用：记录 Codex Status Light P4 发布阶段的实现和验证结果
职责范围：
1. 说明发布 EXE 的子系统、运行时和发布目录策略
2. 记录本机可执行验证结果
3. 标明无法在当前机器完成的实机验收项

不负责：
- 重新定义 P0 事件映射
- 替代后续 Windows 11 / 多缩放比例人工验收报告

维护说明：
- 本文记录当前发布构建事实；发布物以 dist/StatusLight.exe 为准
-->

# P4 Release

Delivered on 2026-08-05.

## Build Shape

```text
Release artifact: dist/StatusLight.exe
Artifact count in dist: 1
Machine type: x64
Subsystem: Windows GUI
Entry point: wmainCRTStartup
CRT: static /MT
Manifest UAC: asInvoker
```

The executable still accepts console diagnostics commands:

```text
StatusLight.exe --status
StatusLight.exe --inspect
```

P4 keeps command output available by using existing stdout/stderr handles when present. When no handles exist, the process attaches or allocates a console only for command-line diagnostic modes.

## Dependency Check

`dumpbin /dependents dist/StatusLight.exe` shows only Windows system DLLs:

```text
KERNEL32.dll
USER32.dll
GDI32.dll
SHELL32.dll
```

No VC runtime DLL dependency was present after switching Release builds to `/MT`.

## Validation

```text
Release x64 build: 0 warnings, 0 errors
dist/StatusLight.exe --status: printed status successfully
No-arg launch: tray process stayed alive for 4 seconds
Cleanup: no StatusLight process left after validation
PE check: machine x64, subsystem Windows GUI
dist contents: StatusLight.exe only
```

## Environment Note

The current validation host reports:

```text
Microsoft Windows NT 10.0.19045.0
```

Windows 11 and multi-DPI visual checks still require a Windows 11 machine with the target scaling settings. The binary is built as a Windows GUI x64 executable and uses Win32 tray/icon APIs, but this workspace cannot truthfully certify Windows 11 visual behavior from the current Windows 10 host.
