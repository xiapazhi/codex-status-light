/**
 * 文件作用：实现 ChatGPT 逻辑对话状态仓库
 * 职责范围：
 * 1. 将同一 conversationId 的多个标签页合并为一个逻辑任务
 * 2. 只在强运行信号开启新 operationGeneration
 * 3. 对同一轮终止事件进行去重
 *
 * 不负责：
 * - 高风险启发式推断等待状态
 * - 保留失效页面的旧红灯或旧黄灯贡献
 *
 * 维护说明：
 * - 标签页关闭、冻结或 snapshot 失败时，必须优先清理旧贡献而不是增加完成数
 */
#include "ChatGptConversationStore.h"

#include <Windows.h>

#include <algorithm>
#include <sstream>

namespace {

int64_t CurrentTimeMs()
{
    FILETIME fileTime {};
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER value {};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return static_cast<int64_t>((value.QuadPart - 116444736000000000ULL) / 10000ULL);
}

std::string ShortReason(const std::string& reason)
{
    return reason.empty() ? "unknown" : reason;
}

bool IsActiveState(WebConversationState state)
{
    return state == WebConversationState::Running || state == WebConversationState::WaitingInput;
}

bool IsTerminalState(WebConversationState state)
{
    return state == WebConversationState::TerminalSuccess ||
        state == WebConversationState::TerminalFailed ||
        state == WebConversationState::TerminalCancelled;
}

} // namespace

bool ChatGptConversationStore::TerminalEventKey::operator<(const TerminalEventKey& other) const
{
    if (conversationKey != other.conversationKey) {
        return conversationKey < other.conversationKey;
    }
    return operationGeneration < other.operationGeneration;
}

std::string ChatGptConversationStore::BuildConversationKey(
    const std::string& browserInstanceId,
    const WebConversationIdentity& identity,
    int tabId,
    const std::string& documentId) const
{
    if (identity.kind == "persistent" && !identity.id.empty()) {
        return browserInstanceId + ":conversation:" + identity.id;
    }

    std::ostringstream key;
    key << browserInstanceId << ":temporary:" << tabId << ":" << documentId;
    return key.str();
}

std::string ChatGptConversationStore::BuildObserverId(
    const std::string& browserInstanceId,
    int tabId,
    const std::string& documentId) const
{
    std::ostringstream key;
    key << browserInstanceId << ":tab:" << tabId << ":" << documentId;
    return key.str();
}

void ChatGptConversationStore::ApplyObservation(const PageObserverRecord& observer)
{
    MigrateConversationIfNeeded(observer);
    observersById_[observer.observerId] = observer;
    WebConversationRecord& conversation = conversationsByKey_[observer.conversationKey];
    if (conversation.conversationKey.empty()) {
        conversation.conversationKey = observer.conversationKey;
        conversation.state = WebConversationState::Idle;
        conversation.stateChangedAt = observer.lastObservedAt != 0 ? observer.lastObservedAt : CurrentTimeMs();
    }

    RebuildConversationObservers();
    if (observer.suspended || !observer.observerHealthy) {
        ClearActiveContributionIfNoHealthyObserver(&conversation, observer.lastObservedAt);
        ChooseOwner(&conversation);
        return;
    }

    ApplyConversationState(observer, &conversation);
    RebuildConversationObservers();
    ChooseOwner(&conversation);
}

void ChatGptConversationStore::RemoveMissingObservers(const std::set<std::string>& currentObserverIds)
{
    bool removed = false;
    for (auto iterator = observersById_.begin(); iterator != observersById_.end();) {
        if (currentObserverIds.find(iterator->first) == currentObserverIds.end()) {
            iterator = observersById_.erase(iterator);
            removed = true;
        } else {
            ++iterator;
        }
    }

    if (!removed) {
        return;
    }

    RebuildConversationObservers();
    for (auto iterator = conversationsByKey_.begin(); iterator != conversationsByKey_.end();) {
        if (iterator->second.observerIds.empty()) {
            iterator = conversationsByKey_.erase(iterator);
            continue;
        }

        if (iterator->second.observerIds.find(iterator->second.activeOwnerObserverId) == iterator->second.observerIds.end()) {
            ChooseOwner(&iterator->second);
        }
        ClearActiveContributionIfNoHealthyObserver(&iterator->second, CurrentTimeMs());
        ++iterator;
    }
}

size_t ChatGptConversationStore::RemoveMissingObserversForBrowser(
    const std::string& browserInstanceId,
    const std::set<std::string>& currentObserverIds)
{
    size_t removedCount = 0;
    std::set<std::string> retainedObserverIds;
    for (const auto& item : observersById_) {
        const bool belongsToBrowser = item.second.browserInstanceId == browserInstanceId;
        const bool observedInSnapshot = currentObserverIds.find(item.first) != currentObserverIds.end();
        if (!belongsToBrowser || observedInSnapshot) {
            retainedObserverIds.insert(item.first);
            continue;
        }
        ++removedCount;
    }

    RemoveMissingObservers(retainedObserverIds);
    return removedCount;
}

void ChatGptConversationStore::RemoveObserver(const std::string& observerId)
{
    std::set<std::string> currentObserverIds;
    for (const auto& item : observersById_) {
        if (item.first != observerId) {
            currentObserverIds.insert(item.first);
        }
    }
    RemoveMissingObservers(currentObserverIds);
}

void ChatGptConversationStore::ClearAll()
{
    observersById_.clear();
    conversationsByKey_.clear();
    handledTerminalEvents_.clear();
    lastReason_.clear();
    lastStateChangedAt_ = 0;
}

std::vector<WebConversationRecord> ChatGptConversationStore::Conversations() const
{
    std::vector<WebConversationRecord> conversations;
    for (const auto& item : conversationsByKey_) {
        conversations.push_back(item.second);
    }
    return conversations;
}

size_t ChatGptConversationStore::ObserverCount() const
{
    return observersById_.size();
}

size_t ChatGptConversationStore::SuspendedObserverCount() const
{
    size_t count = 0;
    for (const auto& item : observersById_) {
        if (item.second.suspended) {
            ++count;
        }
    }
    return count;
}

std::string ChatGptConversationStore::LastReason() const
{
    return lastReason_;
}

int64_t ChatGptConversationStore::LastStateChangedAt() const
{
    return lastStateChangedAt_;
}

void ChatGptConversationStore::MigrateConversationIfNeeded(const PageObserverRecord& observer)
{
    const auto existingObserver = observersById_.find(observer.observerId);
    if (existingObserver == observersById_.end()) {
        return;
    }

    const std::string oldKey = existingObserver->second.conversationKey;
    if (oldKey == observer.conversationKey) {
        return;
    }

    auto oldConversation = conversationsByKey_.find(oldKey);
    if (oldConversation == conversationsByKey_.end()) {
        return;
    }

    WebConversationRecord migrated = oldConversation->second;
    migrated.conversationKey = observer.conversationKey;

    auto newConversation = conversationsByKey_.find(observer.conversationKey);
    if (newConversation == conversationsByKey_.end()) {
        conversationsByKey_[observer.conversationKey] = migrated;
    } else {
        WebConversationRecord& existingConversation = newConversation->second;
        if (existingConversation.state == WebConversationState::Idle || existingConversation.state == WebConversationState::Unknown) {
            existingConversation.state = migrated.state;
            existingConversation.operationGeneration = migrated.operationGeneration;
            existingConversation.operationActive = migrated.operationActive;
            existingConversation.operationSawRunning = migrated.operationSawRunning;
            existingConversation.activeOwnerObserverId = migrated.activeOwnerObserverId;
            existingConversation.activeOwnerBrowserInstanceId = migrated.activeOwnerBrowserInstanceId;
            existingConversation.activeOwnerTabId = migrated.activeOwnerTabId;
            existingConversation.activeOwnerWindowId = migrated.activeOwnerWindowId;
            existingConversation.stateChangedAt = migrated.stateChangedAt;
            existingConversation.lastObservedAt = migrated.lastObservedAt;
            existingConversation.terminalReason = migrated.terminalReason;
        }
    }

    conversationsByKey_.erase(oldConversation);
}

void ChatGptConversationStore::RebuildConversationObservers()
{
    for (auto& item : conversationsByKey_) {
        item.second.observerIds.clear();
    }

    for (const auto& item : observersById_) {
        auto conversation = conversationsByKey_.find(item.second.conversationKey);
        if (conversation != conversationsByKey_.end()) {
            conversation->second.observerIds.insert(item.first);
            conversation->second.lastObservedAt = std::max(conversation->second.lastObservedAt, item.second.lastObservedAt);
        }
    }
}

void ChatGptConversationStore::ApplyConversationState(const PageObserverRecord& observer, WebConversationRecord* conversation)
{
    const WebConversationState nextState = ToConversationState(observer.state);
    if (nextState == WebConversationState::Unknown) {
        return;
    }

    if (nextState == WebConversationState::Running) {
        if (!conversation->operationActive) {
            conversation->operationGeneration++;
            conversation->operationActive = true;
            conversation->operationSawRunning = false;
            conversation->activeOwnerObserverId = observer.observerId;
        }
        conversation->operationSawRunning = true;
        SetConversationState(conversation, WebConversationState::Running, observer.lastObservedAt, observer.reason);
        return;
    }

    if (nextState == WebConversationState::WaitingInput) {
        if (!conversation->operationActive) {
            conversation->operationGeneration++;
            conversation->operationActive = true;
            conversation->operationSawRunning = false;
            conversation->activeOwnerObserverId = observer.observerId;
        }
        SetConversationState(conversation, WebConversationState::WaitingInput, observer.lastObservedAt, observer.reason);
        return;
    }

    if (IsTerminalState(nextState)) {
        if (!conversation->operationActive) {
            return;
        }
        if (!conversation->operationSawRunning) {
            conversation->operationActive = false;
            conversation->operationSawRunning = false;
            conversation->terminalReason.reset();
            SetConversationState(conversation, WebConversationState::Idle, observer.lastObservedAt, "terminal-without-running");
            return;
        }
        TerminalEventKey key { conversation->conversationKey, conversation->operationGeneration };
        if (handledTerminalEvents_.find(key) != handledTerminalEvents_.end()) {
            return;
        }
        handledTerminalEvents_.insert(key);
        conversation->operationActive = false;
        conversation->operationSawRunning = false;
        conversation->terminalReason = TerminalReason { ShortReason(observer.reason) };
        SetConversationState(conversation, nextState, observer.lastObservedAt, observer.reason);
        return;
    }

    if (nextState == WebConversationState::Idle && IsActiveState(conversation->state)) {
        const int64_t observedAt = observer.lastObservedAt != 0 ? observer.lastObservedAt : CurrentTimeMs();
        const bool stableEnough = observedAt - conversation->stateChangedAt >= 2000;
        if (stableEnough) {
            if (!conversation->operationSawRunning) {
                conversation->operationActive = false;
                conversation->operationSawRunning = false;
                conversation->terminalReason.reset();
                SetConversationState(conversation, WebConversationState::Idle, observedAt, "idle-without-running");
                return;
            }
            TerminalEventKey key { conversation->conversationKey, conversation->operationGeneration };
            if (handledTerminalEvents_.find(key) == handledTerminalEvents_.end()) {
                handledTerminalEvents_.insert(key);
                conversation->operationActive = false;
                conversation->operationSawRunning = false;
                conversation->terminalReason = TerminalReason { "stable-idle-after-active" };
                SetConversationState(conversation, WebConversationState::TerminalSuccess, observedAt, "stable-idle-after-active");
            }
        }
        return;
    }

    if (conversation->state == WebConversationState::Unknown) {
        SetConversationState(conversation, WebConversationState::Idle, observer.lastObservedAt, observer.reason);
    }
}

void ChatGptConversationStore::ClearActiveContributionIfNoHealthyObserver(WebConversationRecord* conversation, int64_t observedAt)
{
    bool hasHealthyObserver = false;
    for (const std::string& observerId : conversation->observerIds) {
        const auto observer = observersById_.find(observerId);
        if (observer != observersById_.end() && observer->second.observerHealthy && !observer->second.suspended) {
            hasHealthyObserver = true;
            break;
        }
    }

    if (!hasHealthyObserver && IsActiveState(conversation->state)) {
        conversation->operationActive = false;
        conversation->operationSawRunning = false;
        conversation->terminalReason.reset();
        SetConversationState(conversation, WebConversationState::Idle, observedAt, "observer-unavailable");
    }
}

void ChatGptConversationStore::ChooseOwner(WebConversationRecord* conversation) const
{
    if (conversation->observerIds.empty()) {
        conversation->activeOwnerObserverId.clear();
        conversation->activeOwnerBrowserInstanceId.clear();
        conversation->activeOwnerTabId = 0;
        conversation->activeOwnerWindowId = 0;
        return;
    }

    auto bestObserver = observersById_.end();
    for (const std::string& observerId : conversation->observerIds) {
        auto item = observersById_.find(observerId);
        if (item == observersById_.end() || item->second.suspended || !item->second.observerHealthy) {
            continue;
        }
        if (bestObserver == observersById_.end() ||
            item->second.lastStrongSignalAt > bestObserver->second.lastStrongSignalAt ||
            item->second.lastObservedAt > bestObserver->second.lastObservedAt) {
            bestObserver = item;
        }
    }

    if (bestObserver != observersById_.end()) {
        conversation->activeOwnerObserverId = bestObserver->first;
        conversation->activeOwnerBrowserInstanceId = bestObserver->second.browserInstanceId;
        conversation->activeOwnerTabId = bestObserver->second.tabId;
        conversation->activeOwnerWindowId = bestObserver->second.windowId;
        return;
    }

    conversation->activeOwnerObserverId = *conversation->observerIds.begin();
    const auto fallbackObserver = observersById_.find(conversation->activeOwnerObserverId);
    if (fallbackObserver != observersById_.end()) {
        conversation->activeOwnerBrowserInstanceId = fallbackObserver->second.browserInstanceId;
        conversation->activeOwnerTabId = fallbackObserver->second.tabId;
        conversation->activeOwnerWindowId = fallbackObserver->second.windowId;
    }
}

void ChatGptConversationStore::SetConversationState(
    WebConversationRecord* conversation,
    WebConversationState state,
    int64_t observedAt,
    const std::string& reason)
{
    if (observedAt == 0) {
        observedAt = CurrentTimeMs();
    }

    if (conversation->state != state) {
        conversation->state = state;
        conversation->stateChangedAt = observedAt;
        lastStateChangedAt_ = observedAt;
        lastReason_ = ShortReason(reason);
    }
    conversation->lastObservedAt = observedAt;
}

WebConversationState ChatGptConversationStore::ToConversationState(WebObservedPageState state) const
{
    switch (state) {
    case WebObservedPageState::Idle:
        return WebConversationState::Idle;
    case WebObservedPageState::Running:
        return WebConversationState::Running;
    case WebObservedPageState::WaitingInput:
        return WebConversationState::WaitingInput;
    case WebObservedPageState::TerminalSuccess:
        return WebConversationState::TerminalSuccess;
    case WebObservedPageState::TerminalFailed:
        return WebConversationState::TerminalFailed;
    case WebObservedPageState::TerminalCancelled:
        return WebConversationState::TerminalCancelled;
    case WebObservedPageState::Unknown:
    default:
        return WebConversationState::Unknown;
    }
}
