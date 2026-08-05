<!--
文件作用：记录 Codex Status Light P0 阶段的真实 JSONL 事件映射
职责范围：
1. 固化已经在本机 rollout JSONL 中验证过的事件名称和字段路径
2. 说明统一事件类型和状态转换依据
3. 标记尚未稳定验证或不能主观推断的场景

不负责：
- 保存用户提示词、模型回复正文或工具输出正文
- 作为完整产品需求文档替代开发指导书

维护说明：
- 后续正式状态机只能依赖本文件中已经验证的稳定映射
-->

# Codex JSONL 事件映射表

取证时间：2026-08-04

取证范围：本机 `%USERPROFILE%\.codex\sessions\**\rollout-*.jsonl`，最近修改的 30 个 rollout 文件。

隐私处理：取证过程只读取事件类型、时间、会话标识、字段名称和少量安全状态字段；不输出 `prompt`、`message`、`content`、`text`、`output`、`stdout`、`stderr`、`arguments`、`encrypted_content`、`base_instructions` 的正文。

取证工具：

```text
StatusLight.exe
StatusLight.exe --tray
StatusLight.exe --status
StatusLight.exe --status --watch
StatusLight.exe --inspect
StatusLight.exe --inspect --watch
StatusLight.exe --inspect --codex-home "D:\path\.codex" --max-files 30 --recent-hours 24
```

直接双击 `StatusLight.exe` 等价于 `StatusLight.exe --tray`：启动 Win32 托盘图标，默认每 2 秒读取最近 5 个 rollout 快照并更新图标。

P2 托盘行为：

```text
中心灰色：空闲或无活动任务
中心黄色：存在运行中的任务
中心红色：存在明确的 request_permissions 或 request_user_input
中心绿色：存在未清除的完成任务
红色闪烁：存在失败任务
灰色加橙色叹号：状态数据异常或无法确认
外围圆环：额度剩余；额度未知时显示灰色虚线环
```

右键菜单：

```text
打开 Codex
清除完成提示
复制诊断信息
退出
```

双击托盘图标会尝试打开 Codex；如果当前是绿色完成状态，会同时清除完成提示。

`--status` 是 P1 控制台解析器，输出示例：

```text
Status summary
Parsed lines: 335, JSON errors: 0, Unknown events: 44
Session 019fcc27-402f-7b80-83b8-c5709e7f25b5: RUNNING turn=... last=function_call
Counts: waiting=0, running=1, completed=0, failed=0, cancelled=0, unknown=0
Quota: partial, effective remaining=80%, primary remaining=80%, plan=...
```

`--watch` 会按文件维护字节游标，只输出新增的完整 JSONL 行；文件末尾未换行时暂不解析该半行，下一轮继续读取。

## 样本概览

| 顶层事件类型 | 样本数量 | 说明 |
| --- | ---: | --- |
| `session_meta` | 165 | 会话元数据，包含 `payload.id`。 |
| `event_msg` | 16900 | 状态流事件，真实任务开始、完成、取消、额度更新主要在这里。 |
| `response_item` | 27690 | 模型消息、推理、工具调用和工具输出。 |
| `turn_context` | 356 | turn 级上下文，包含批准策略等字段，但不是任务状态事件。 |
| `world_state` | 122 | 宿主状态快照，不作为任务状态转换依据。 |
| `compacted` | 75 | 上下文压缩事件，不作为任务状态转换依据。 |

## 已验证事件映射

| 原始事件名称 | 关键字段路径 | 统一事件类型 | 状态转换 | 样本来源 | 验证场景 | 兼容性备注 |
| --- | --- | --- | --- | --- | --- | --- |
| `session_meta` | `type`, `timestamp`, `payload.id` | `SessionDiscovered` | 创建或刷新会话记录，不直接进入 Running | `rollout-2026-08-04T17-42-33-019fcc27-402f-7b80-83b8-c5709e7f25b5.jsonl:1` | 普通任务开始前的会话发现 | `sessionId` 优先取 `payload.id`；取不到时可从文件名兜底。 |
| `task_started` | `type=event_msg`, `payload.type`, `payload.turn_id`, `payload.started_at` | `TaskStarted` | `Running` | `rollout-2026-08-04T17-42-33-019fcc27-402f-7b80-83b8-c5709e7f25b5.jsonl:2` | 普通任务开始 | 这是当前样本中最稳定的任务开始事件。 |
| `reasoning` | `type=response_item`, `payload.type=reasoning`, `payload.id` | `ModelActivity` | `Running` | `rollout-2026-08-04T17-42-33-019fcc27-402f-7b80-83b8-c5709e7f25b5.jsonl:10` | 模型开始思考 | 正文位于 `payload.encrypted_content` 或相关内容字段，取证工具只记录字段存在。 |
| `agent_reasoning` | `type=event_msg`, `payload.type`, `payload.text` | `ModelActivity` | `Running` | `rollout-2026-08-04T17-42-33-019fcc27-402f-7b80-83b8-c5709e7f25b5.jsonl:34` | 普通思考状态流 | `payload.text` 不输出；只用事件类型判断。 |
| `agent_message` | `type=event_msg`, `payload.type`, `payload.phase`, `payload.memory_citation` | `ModelActivity` | `Running` | `rollout-2026-08-04T17-42-33-019fcc27-402f-7b80-83b8-c5709e7f25b5.jsonl:11` | 模型状态消息 | 该事件可说明模型仍在产出，但不能替代 `task_complete` 判断完成。 |
| `function_call` | `type=response_item`, `payload.type`, `payload.name`, `payload.call_id` | `ToolStarted` | `Running` | `rollout-2026-08-04T17-42-33-019fcc27-402f-7b80-83b8-c5709e7f25b5.jsonl:13` | 工具调用开始、命令执行 | `payload.name` 可区分 `shell_command`、`update_plan`、`request_user_input` 等工具。 |
| `custom_tool_call` | `type=response_item`, `payload.type`, `payload.name`, `payload.call_id`, `payload.status` | `ToolStarted` | `Running` | `rollout-2026-08-04T09-35-30-019fca69-558f-75c1-a8e4-162056b0c30e.jsonl:14` | 自定义工具调用开始 | 与 `function_call` 同类处理。 |
| `function_call_output` | `type=response_item`, `payload.type`, `payload.call_id`, `payload.output` | `ToolCompleted` | 保持或回到 `Running`，不单独判定完成 | `rollout-2026-08-04T17-42-33-019fcc27-402f-7b80-83b8-c5709e7f25b5.jsonl:14` | 工具调用结束 | `payload.output` 不输出；必须用 `call_id` 与调用关联。 |
| `custom_tool_call_output` | `type=response_item`, `payload.type`, `payload.call_id`, `payload.output` | `ToolCompleted` | 保持或回到 `Running`，不单独判定完成 | `rollout-2026-08-04T09-35-30-019fca69-558f-75c1-a8e4-162056b0c30e.jsonl:15` | 自定义工具调用结束 | 与 `function_call_output` 同类处理。 |
| `patch_apply_end` | `type=event_msg`, `payload.type`, `payload.call_id`, `payload.status`, `payload.success` | `ToolCompleted` | 成功时保持 `Running`；失败时记录工具失败但不直接等同任务失败 | `rollout-2026-08-04T09-35-30-019fca69-558f-75c1-a8e4-162056b0c30e.jsonl:231` | 文件修改工具结束 | 样本中 `payload.status=failed` 出现过 2 次，应进入诊断计数。 |
| `mcp_tool_call_end` | `type=event_msg`, `payload.type`, `payload.call_id`, `payload.duration`, `payload.result` | `ToolCompleted` | 保持或回到 `Running` | `rollout-2026-08-04T09-35-30-019fca69-558f-75c1-a8e4-162056b0c30e.jsonl:724` | MCP 工具调用结束 | `payload.result` 不输出。 |
| `web_search_call` | `type=response_item`, `payload.type`, `payload.call_id` | `ToolStarted` | `Running` | `rollout-2026-07-08T15-10-50-019f4090-a8a5-76f0-a7c4-7fa33bc66892.jsonl:1072` | Web 搜索开始 | 第一版不主动访问网络，只读取本地事件。 |
| `web_search_end` | `type=event_msg`, `payload.type`, `payload.call_id` | `ToolCompleted` | 保持或回到 `Running` | `rollout-2026-08-04T14-25-31-019fcb72-e17c-7d23-8420-70965f9885c0.jsonl:55` | Web 搜索结束 | 只记录事件存在，不读取搜索结果正文。 |
| `request_permissions` | `type=response_item`, `payload.type=function_call`, `payload.name`, `payload.call_id`, `payload.arguments` | `WaitingForApproval` | `WaitingInput` | `rollout-2026-07-27T08-54-15-019fa110-b679-7fb1-8ff7-917e07a0e40b.jsonl:95` | 请求执行命令批准、请求修改文件批准 | 权限批准请求会进入 rollout JSONL；不要解析 `payload.arguments` 正文。 |
| `request_user_input` | `type=response_item`, `payload.type=function_call`, `payload.name`, `payload.call_id`, `payload.arguments` | `WaitingForUserInput` | `WaitingInput` | `rollout-2026-08-04T09-35-30-019fca69-558f-75c1-a8e4-162056b0c30e.jsonl:128` | Codex 主动询问用户 | 主动询问用户有明确事件；取证工具只识别工具名。 |
| `user_message` | `type=event_msg`, `payload.type`, `payload.client_id`, `payload.images`, `payload.local_images` | `UserInputReceived` | `Running` | `rollout-2026-08-04T17-42-33-019fcc27-402f-7b80-83b8-c5709e7f25b5.jsonl:9` | 用户回答后继续运行 | `payload.message`、`payload.text_elements` 不输出。 |
| `task_complete` | `type=event_msg`, `payload.type`, `payload.turn_id`, `payload.completed_at`, `payload.duration_ms` | `TaskCompleted` | `Completed` | `rollout-2026-08-04T09-35-30-019fca69-558f-75c1-a8e4-162056b0c30e.jsonl:76` | 任务正常完成 | 若同事件包含 `payload.error`，必须转为 `TaskFailed`。 |
| `task_complete` with `error` | `type=event_msg`, `payload.type`, `payload.error`, `payload.completed_at`, `payload.duration_ms` | `TaskFailed` | `Failed` | `rollout-2026-07-08T15-10-50-019f4090-a8a5-76f0-a7c4-7fa33bc66892.jsonl:22502` | 任务失败 | 失败不是独立顶层事件名，而是 `task_complete` 携带 `error` 字段。 |
| `turn_aborted` | `type=event_msg`, `payload.type`, `payload.reason`, `payload.completed_at`, `payload.duration_ms` | `TaskCancelled` | `Cancelled` | `rollout-2026-08-04T09-35-30-019fca69-558f-75c1-a8e4-162056b0c30e.jsonl:824` | 用户主动取消或 turn 中止 | `payload.reason` 可用于诊断，但不能输出正文类字段。 |
| `token_count` | `type=event_msg`, `payload.type`, `payload.info`, `payload.rate_limits` | `QuotaUpdated` | 不改变任务中心状态，只更新额度快照 | `rollout-2026-08-04T17-42-33-019fcc27-402f-7b80-83b8-c5709e7f25b5.jsonl:15` | 额度数据更新 | 额度未知或不完整不能触发右下角叹号。 |

## 额度字段结构

已验证 `token_count` 事件中的真实字段结构：

```text
payload.rate_limits.credits
payload.rate_limits.individual_limit
payload.rate_limits.limit_id
payload.rate_limits.limit_name
payload.rate_limits.plan_type
payload.rate_limits.primary.used_percent
payload.rate_limits.primary.window_minutes
payload.rate_limits.primary.resets_at
payload.rate_limits.secondary.used_percent
payload.rate_limits.secondary.window_minutes
payload.rate_limits.secondary.resets_at
payload.rate_limits.rate_limit_reached_type
payload.rate_limits.spend_control_reached
```

样本中存在三类结构：

| 结构 | 样本数量 | 结论 |
| --- | ---: | --- |
| `credits,individual_limit,limit_id,limit_name,plan_type,primary,rate_limit_reached_type,secondary` | 3390 | 可解析主窗口和周窗口；若两个窗口都有 `used_percent`，外环取更低剩余值。 |
| `credits,individual_limit,limit_id,limit_name,plan_type,primary,rate_limit_reached_type,secondary,spend_control_reached` | 4315 | 同上，额外记录 spend control 状态用于诊断。 |
| 空 `rate_limits` 或不可展开 | 27 | 标记为 `Unavailable` 或 `Ambiguous`，只影响额度环，不影响任务中心状态。 |

验证样本：

```text
primary + secondary: rollout-2026-06-26T13-14-27-019f0259-b8bd-7dd3-a527-7ce1103e4a0b.jsonl:22
primary only / secondary empty: rollout-2026-08-04T17-42-33-019fcc27-402f-7b80-83b8-c5709e7f25b5.jsonl:15
```

## 会话与任务关联

已验证字段：

```text
session_meta.payload.id
event_msg.payload.thread_id
event_msg.payload.turn_id
response_item.payload.call_id
```

当前结论：

1. 会话优先使用 `session_meta.payload.id`。
2. 若事件携带 `payload.thread_id`，可用它覆盖文件名推断出的会话标识。
3. turn 级状态使用 `payload.turn_id` 关联。
4. 工具调用开始和结束使用 `payload.call_id` 关联。
5. 文件名中的 UUID 可作为兜底会话标识，但不是首选结构化字段。

## 已回答的问题

| 问题 | 结论 |
| --- | --- |
| Codex Desktop 的真实任务开始事件是什么？ | `event_msg.payload.type=task_started`。 |
| 普通思考和工具执行分别有哪些事件？ | 普通思考：`response_item.payload.type=reasoning`、`event_msg.payload.type=agent_reasoning`；工具执行：`function_call`、`custom_tool_call`、`tool_search_call`、`web_search_call` 以及对应输出事件。 |
| 权限批准请求是否进入 rollout JSONL？ | 是，表现为 `response_item.payload.type=function_call` 且 `payload.name=request_permissions`。 |
| 主动询问用户是否有明确事件？ | 是，表现为 `response_item.payload.type=function_call` 且 `payload.name=request_user_input`。 |
| 完成、失败和取消分别使用什么事件？ | 完成：`task_complete`；失败：`task_complete` 携带 `error` 字段；取消：`turn_aborted`。 |
| `rate_limits` 的真实字段结构是什么？ | 位于 `event_msg.payload.rate_limits`，包含 `primary`、`secondary`、`plan_type`、`limit_id` 等字段；窗口内字段为 `used_percent`、`window_minutes`、`resets_at`。 |
| 不同任务和会话如何关联？ | 会话用 `payload.id` / `payload.thread_id` / 文件名兜底；turn 用 `payload.turn_id`；工具调用用 `payload.call_id`。 |

## 暂未稳定验证的场景

| 场景 | 当前处理 |
| --- | --- |
| 额度耗尽 | 已发现 `rate_limit_reached_type` 和 `spend_control_reached` 字段，但未在本轮样本中确认完整耗尽状态机，不强行映射为任务失败。 |
| 网络错误 | 可能出现在工具输出正文或 `task_complete.error` 中；P0 工具不读取正文，因此只记录失败字段存在，不做错误类型细分。 |
| Codex 进程退出 | rollout JSONL 样本中未确认稳定结构化事件；第一版只能由后续 `ProcessMonitor` 使用 Win32 进程快照辅助判断。 |
| Desktop 与 CLI 同时运行 | 本轮样本能看到多 rollout 并存，但未区分来源为 Desktop 或 CLI 的稳定字段；不能主观映射。 |
| 所有等待人工状态 | 已验证 `request_permissions` 和 `request_user_input`；其他等待状态必须继续进入 Unknown，直到取得真实样本。 |

## 对正式状态机的约束

1. 只有 `request_permissions` 和 `request_user_input` 能进入红灯等待状态。
2. 文件长时间没有更新不能推断为等待用户。
3. 工具输出失败只记录为工具失败；只有 `task_complete.error` 才能将任务置为 `Failed`。
4. `token_count` 只更新额度，不改变任务状态。
5. `turn_context`、`world_state`、`compacted`、`thread_settings_applied`、`thread_goal_updated`、`thread_rolled_back` 默认不参与红黄绿状态转换。
6. 新事件或字段结构不在本表时必须进入 `Unknown`，并保存脱敏样本供后续验证。
