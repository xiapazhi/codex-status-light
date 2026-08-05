/**
 * 文件作用：定义 ChatGPT Web 扩展桥接的共享数据模型
 * 职责范围：
 * 1. 表达 Native Messaging、页面观察、逻辑对话和账号聚合状态
 * 2. 为浏览器扩展桥接模块和托盘聚合层提供稳定的数据边界
 *
 * 不负责：
 * - 读取 ChatGPT DOM 或执行浏览器脚本
 * - 判断 Codex 本地 JSONL 状态
 *
 * 维护说明：
 * - 标签页只作为观察器，只有 WebConversationRecord 参与任务计数
 */
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

enum class WebConversationState {
    Unknown,
    Idle,
    Running,
    WaitingInput,
    TerminalSuccess,
    TerminalFailed,
    TerminalCancelled
};

enum class WebObservedPageState {
    Unknown,
    Idle,
    Running,
    WaitingInput,
    TerminalSuccess,
    TerminalFailed,
    TerminalCancelled
};

enum class WebMonitorHealth {
    Normal,
    Degraded,
    Error
};

struct BrowserProfileScope {
    std::string scopeId;
    std::string browserInstanceId;
};

struct WebConversationIdentity {
    std::string kind;
    std::string id;
};

struct PageObserverRecord {
    std::string observerId;
    std::string browserInstanceId;
    int tabId = 0;
    int windowId = 0;
    std::string documentId;
    std::string conversationKey;
    WebObservedPageState state = WebObservedPageState::Unknown;
    std::string reason;
    int64_t lastObservedAt = 0;
    int64_t lastStrongSignalAt = 0;
    bool visible = false;
    bool focused = false;
    bool suspended = false;
    bool observerHealthy = false;
};

struct TerminalReason {
    std::string reason;
};

struct WebConversationRecord {
    std::string conversationKey;
    std::set<std::string> observerIds;
    std::string activeOwnerObserverId;
    WebConversationState state = WebConversationState::Unknown;
    uint64_t operationGeneration = 0;
    bool operationActive = false;
    int64_t stateChangedAt = 0;
    int64_t lastObservedAt = 0;
    std::optional<TerminalReason> terminalReason;
};

struct WebAccountState {
    uint32_t waitingCount = 0;
    uint32_t runningCount = 0;
    uint32_t completedCount = 0;
    WebMonitorHealth health = WebMonitorHealth::Normal;
    std::vector<WebConversationRecord> conversations;
    size_t chromeProfileScopes = 0;
    size_t chatGptTabs = 0;
    size_t duplicateObservers = 0;
    size_t activePageObservers = 0;
    size_t suspendedObservers = 0;
    size_t nativeBridgeClients = 0;
    bool nativeHostRegistered = false;
    size_t protocolErrorCount = 0;
    size_t nativeReconnectAttempts = 0;
    size_t observerReinstallCount = 0;
    std::string lastReason;
    int64_t lastStateChangedAt = 0;
    std::string bridgeState;
    std::wstring diagnosticMessage;
};
