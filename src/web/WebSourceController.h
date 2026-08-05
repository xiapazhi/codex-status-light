/**
 * 文件作用：声明 ChatGPT Web 扩展来源控制器
 * 职责范围：
 * 1. 管理主进程 Named Pipe 服务启停
 * 2. 接收扩展上报的结构化标签页状态
 * 3. 复用 ConversationStore/Aggregator 输出账号级计数
 *
 * 不负责：
 * - 观察 ChatGPT DOM
 * - Native Messaging stdio 子进程转发
 *
 * 维护说明：
 * - Content Script 属于低信任来源，所有字段必须校验后再进入状态仓库
 */
#pragma once

#include "ChatGptAccountAggregator.h"
#include "ChatGptConversationStore.h"
#include "WebBridgePipeServer.h"
#include "WebDiagnostics.h"
#include "NativeHostRegistration.h"

#include "../JsonValue.h"

#include <map>
#include <mutex>
#include <set>
#include <string>

class WebSourceController {
public:
    void Enable();
    void Disable();
    bool IsEnabled() const;
    void PollOnce();
    WebAccountState CurrentState() const;
    std::wstring Diagnostics() const;

private:
    mutable std::mutex mutex_;
    bool enabled_ = false;
    ChatGptConversationStore store_;
    ChatGptAccountAggregator aggregator_;
    WebDiagnostics diagnostics_;
    NativeHostRegistration registration_;
    WebBridgePipeServer pipeServer_;
    WebAccountState currentState_;
    std::map<std::string, uint64_t> lastSequenceByObserver_;
    std::map<std::string, std::set<std::string>> observersByTab_;
    std::set<std::string> browserInstances_;
    size_t protocolErrorCount_ = 0;
    size_t reconnectAttempts_ = 0;
    size_t noClientPollCount_ = 0;
    bool nativeHostRegistered_ = false;

    std::string HandleBridgeMessage(const std::string& message);
    std::string HandleParsedMessage(const JsonValue& root);
    std::string ApplyTabState(const JsonValue& root);
    std::string ApplyTabRemoved(const JsonValue& root);
    std::string ApplyTabSuspended(const JsonValue& root);
    void RefreshStateLocked(WebMonitorHealth health, const std::wstring& diagnosticMessage);
    bool ReadRequiredString(const JsonValue& root, const std::string& name, std::string* value);
    bool ReadRequiredInt(const JsonValue& root, const std::string& name, int* value);
    bool ReadOptionalUint64(const JsonValue& root, const std::string& name, uint64_t* value);
    WebObservedPageState ParseObservedState(const std::string& state) const;
    std::string TabKey(const std::string& browserInstanceId, int tabId) const;
};
