/**
 * 文件作用：声明 ChatGPT Web 账号级聚合器
 * 职责范围：
 * 1. 将逻辑对话状态聚合为等待、运行、完成三个计数
 * 2. 计算重复观察器数量和健康状态
 *
 * 不负责：
 * - 合并 Codex 本地任务
 * - 控制托盘动画
 *
 * 维护说明：
 * - 失败、取消和其他终止态均归入 completedCount，不增加第四种主颜色
 */
#pragma once

#include "WebTypes.h"

#include <vector>

class ChatGptAccountAggregator {
public:
    WebAccountState Aggregate(
        const std::vector<WebConversationRecord>& conversations,
        size_t profileScopes,
        size_t tabCount,
        size_t observerCount,
        size_t suspendedObserverCount,
        WebMonitorHealth health) const;
};
