<!--
文件作用：说明 StatusLight Web Companion 与 StatusLight.exe 的桥接协议
职责范围：
1. 记录 Native Messaging Host 名称和消息帧格式
2. 说明扩展到主进程的 P0 消息
3. 标明隐私边界和当前开发限制

不负责：
- 记录 ChatGPT DOM 选择器细节
- 替代 Chrome Web Store 发布说明

维护说明：
- 修改协议字段时必须同步 extension/ 和 src/web/
-->

# Web Extension Protocol

Native Host:

```text
com.statuslight.web
```

本地链路：

```text
Content Script -> Service Worker -> Native Messaging -> StatusLight.exe Native Host -> Named Pipe -> StatusLight.exe main process
```

帧格式：

```text
uint32 little-endian length
UTF-8 JSON payload
```

限制：

```text
Max input: 64 KB
Max output: 64 KB
```

P0 消息：

```json
{ "protocolVersion": 1, "type": "ping" }
```

返回：

```json
{ "protocolVersion": 1, "type": "pong" }
```

第一版固定开发扩展来源：

```text
chrome-extension://pkaefmgibeeemjoilbpopeiffmkbnjoi/
```

`extension/manifest.json` 已写入固定 `"key"`，加载解压扩展时应得到同一个开发版扩展 ID。
