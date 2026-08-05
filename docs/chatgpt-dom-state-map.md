<!--
文件作用：记录 ChatGPT DOM 状态判断规则
职责范围：
1. 说明第一版运行、等待、终止状态的选择器边界
2. 标记后续需要真实采样验证的场景

不负责：
- 保存聊天正文或页面标题
- 记录用户账号信息

维护说明：
- 新增选择器必须先确认不会把侧边栏、历史列表或普通问句误判为任务状态
-->

# ChatGPT DOM State Map

当前扩展只发送固定枚举：

```text
idle
running
waiting
terminal_success
terminal_failed
terminal_cancelled
unknown
```

运行强信号：

```text
button[data-testid="stop-button"]
button[aria-label="Stop streaming"]
button[aria-label="Stop generating"]
main 内 aria-busy="true"
main 内 role="progressbar"
```

等待信号：

```text
任务区域内 approve / confirm / continue 类 data-testid
radio
checkbox
select
```

排除原则：

```text
不通过问号判断等待
不发送按钮原始文字
不发送页面标题、聊天标题、正文、Cookie、Storage 或 Network 数据
```
