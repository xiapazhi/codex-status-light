<!--
文件作用：说明 Codex Status Light 项目的用途、构建方式和运行方式
职责范围：
1. 描述项目目标和主要状态含义
2. 记录 Windows / MSVC / CMake 构建命令
3. 说明发布产物和命令行调试入口

不负责：
- 记录 Codex JSONL 事件完整取证细节
- 替代 docs 目录中的阶段实现文档

维护说明：
- 面向开源仓库首页，保持简洁、可验证、可直接上手
-->

# Codex Status Light

Codex Status Light is a lightweight Windows tray indicator for Codex task status.

It reads local Codex JSONL session files from `%USERPROFILE%\.codex\sessions\**\rollout-*.jsonl` and can optionally merge ChatGPT Web task state reported by the companion Chrome MV3 extension.

## Status

```text
Waiting   Red, flashes quickly for 3 seconds, then stays red.
Running   Bright amber breathing animation.
Done      Green, flashes quickly for 1 second, then stays green.
```

Priority:

```text
Waiting > Running > Done
```

The quota ring is independent from the center status color.

## Build

Requirements:

```text
Windows
MSVC x64
CMake
C++17
```

Build Release:

```bat
cmake -S . -B build -G "Visual Studio 15 2017 Win64"
cmake --build build --config Release
```

The release executable is copied to:

```text
dist\StatusLight.exe
```

## Usage

Double click `StatusLight.exe` to start tray mode.

Console diagnostics:

```bat
StatusLight.exe --status
StatusLight.exe --inspect
StatusLight.exe --self-test-web
```

Optional Codex home override:

```bat
StatusLight.exe --status --codex-home "D:\path\.codex"
```

ChatGPT Web monitoring is enabled or disabled from the tray context menu. The browser side is a small MV3 extension in `extension/`, and the desktop side uses Chrome Native Messaging plus a per-user Named Pipe. It does not use CDP, a remote debugging port, Node.js, localhost HTTP, cookies, browser storage, or chat content.

Native Host:

```text
com.statuslight.web
```

Development setup:

```text
1. Start StatusLight.exe and enable the web bridge from the tray menu.
2. StatusLight.exe writes the Native Host manifest and HKCU registration automatically.
3. Load extension\ as an unpacked Chrome extension.
4. Confirm Chrome shows extension ID pkaefmgibeeemjoilbpopeiffmkbnjoi.
```

See `docs\web-extension-protocol.md` and `docs\chatgpt-dom-state-map.md` for the protocol and DOM-state boundary.
