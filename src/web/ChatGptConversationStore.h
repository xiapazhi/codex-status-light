/**
 * 文件作用：声明 ChatGPT 逻辑对话状态仓库
 * 职责范围：
 * 1. 按 conversationKey 合并多个页面观察器
 * 2. 维护 Owner、operationGeneration 和终止去重
 * 3. 移除失效观察器带来的过期运行/等待贡献
 *
 * 不负责：
 * - 连接 Chrome Native Messaging 或读取扩展消息
 * - 绘制托盘图标
 *
 * 维护说明：
 * - 任务计数只能来自 WebConversationRecord，不能直接来自标签页数量
 */
#pragma once

#include "WebTypes.h"

#include <map>
#include <set>
#include <string>
#include <vector>

class ChatGptConversationStore {
public:
    std::string BuildConversationKey(
        const std::string& browserInstanceId,
        const WebConversationIdentity& identity,
        int tabId,
        const std::string& documentId) const;
    std::string BuildObserverId(
        const std::string& browserInstanceId,
        int tabId,
        const std::string& documentId) const;

    void ApplyObservation(const PageObserverRecord& observer);
    void RemoveMissingObservers(const std::set<std::string>& currentObserverIds);
    void RemoveObserver(const std::string& observerId);
    void ClearAll();
    std::vector<WebConversationRecord> Conversations() const;
    size_t ObserverCount() const;
    size_t SuspendedObserverCount() const;
    std::string LastReason() const;
    int64_t LastStateChangedAt() const;

private:
    struct TerminalEventKey {
        std::string conversationKey;
        uint64_t operationGeneration = 0;

        bool operator<(const TerminalEventKey& other) const;
    };

    std::map<std::string, PageObserverRecord> observersById_;
    std::map<std::string, WebConversationRecord> conversationsByKey_;
    std::set<TerminalEventKey> handledTerminalEvents_;
    std::string lastReason_;
    int64_t lastStateChangedAt_ = 0;

    void MigrateConversationIfNeeded(const PageObserverRecord& observer);
    void RebuildConversationObservers();
    void ApplyConversationState(const PageObserverRecord& observer, WebConversationRecord* conversation);
    void ClearActiveContributionIfNoHealthyObserver(WebConversationRecord* conversation, int64_t observedAt);
    void ChooseOwner(WebConversationRecord* conversation) const;
    void SetConversationState(
        WebConversationRecord* conversation,
        WebConversationState state,
        int64_t observedAt,
        const std::string& reason);
    WebConversationState ToConversationState(WebObservedPageState state) const;
};
