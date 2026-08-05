/**
 * 文件作用：实现 ChatGPT Web 账号级聚合器
 * 职责范围：
 * 1. 按 Waiting > Running > Completed 的三色模型统计逻辑对话
 * 2. 计算诊断所需的技术数量
 *
 * 不负责：
 * - 判断页面 DOM
 * - 触发通知或动画
 *
 * 维护说明：
 * - Tooltip 使用任务计数，标签页和 Observer 数量只进入诊断
 */
#include "ChatGptAccountAggregator.h"

WebAccountState ChatGptAccountAggregator::Aggregate(
    const std::vector<WebConversationRecord>& conversations,
    size_t profileScopes,
    size_t tabCount,
    size_t observerCount,
    size_t suspendedObserverCount,
    WebMonitorHealth health) const
{
    WebAccountState state;
    state.health = health;
    state.conversations = conversations;
    state.chromeProfileScopes = profileScopes;
    state.chatGptTabs = tabCount;
    state.activePageObservers = observerCount;
    state.suspendedObservers = suspendedObserverCount;
    state.duplicateObservers = observerCount > conversations.size() ? observerCount - conversations.size() : 0;

    for (const WebConversationRecord& conversation : conversations) {
        switch (conversation.state) {
        case WebConversationState::WaitingInput:
            ++state.waitingCount;
            break;
        case WebConversationState::Running:
            ++state.runningCount;
            break;
        case WebConversationState::TerminalSuccess:
        case WebConversationState::TerminalFailed:
        case WebConversationState::TerminalCancelled:
            ++state.completedCount;
            break;
        case WebConversationState::Unknown:
        case WebConversationState::Idle:
        default:
            break;
        }
    }

    return state;
}
