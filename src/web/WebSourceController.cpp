/**
 * 文件作用：实现 ChatGPT Web 扩展来源控制器
 * 职责范围：
 * 1. 启动和停止 WebBridge Named Pipe 服务
 * 2. 将扩展协议消息转换为 PageObserverRecord
 * 3. 输出 WebAccountState 供托盘统一聚合
 *
 * 不负责：
 * - 执行 DOM 选择器或页面状态启发式判断
 * - 读取 Cookie、Storage、Network 或聊天正文
 *
 * 维护说明：
 * - 协议错误只计入诊断，不允许让主进程崩溃
 */
#include "WebSourceController.h"

#include "NativeHostProtocol.h"
#include "WebDebugLog.h"

#include "../JsonValue.h"

#include <Windows.h>

#include <sstream>

namespace {

constexpr size_t kMaxNoClientPollsBeforeError = 5;

int64_t CurrentTimeMs()
{
    FILETIME fileTime {};
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER value {};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return static_cast<int64_t>((value.QuadPart - 116444736000000000ULL) / 10000ULL);
}

std::string Ack()
{
    return NativeHostProtocol::MakeMessage("ack");
}

std::string Pong()
{
    return NativeHostProtocol::MakeMessage("pong");
}

std::wstring WideCount(const wchar_t* label, size_t value)
{
    std::wostringstream output;
    output << label << value;
    return output.str();
}

} // namespace

void WebSourceController::Enable()
{
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = true;
    reconnectAttempts_ = 0;
    noClientPollCount_ = 0;
    std::wstring errorMessage;
    nativeHostRegistered_ = registration_.EnsureRegistered(&errorMessage);
    const bool started = pipeServer_.Start(
        [this](const std::string& message) {
            return HandleBridgeMessage(message);
        },
        &errorMessage);
    const WebMonitorHealth health = started && nativeHostRegistered_
        ? WebMonitorHealth::Normal
        : WebMonitorHealth::Error;
    std::wostringstream output;
    output
        << L"enable registered=" << (nativeHostRegistered_ ? L"true" : L"false")
        << L" pipe_started=" << (started ? L"true" : L"false");
    if (!errorMessage.empty()) {
        output << L" error=" << errorMessage;
    }
    WebDebugLog::Write(L"WebSource", output.str());
    RefreshStateLocked(health, errorMessage);
}

void WebSourceController::Disable()
{
    pipeServer_.Stop();

    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = false;
    store_.ClearAll();
    browserInstances_.clear();
    observersByTab_.clear();
    lastSequenceByObserver_.clear();
    reconnectAttempts_ = 0;
    noClientPollCount_ = 0;
    RefreshStateLocked(WebMonitorHealth::Normal, L"");
    WebDebugLog::Write(L"WebSource", L"disabled and state cleared");
}

bool WebSourceController::IsEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

void WebSourceController::PollOnce()
{
    std::lock_guard<std::mutex> lock(mutex_);
    WebMonitorHealth health = pipeServer_.IsRunning() ? WebMonitorHealth::Normal : WebMonitorHealth::Error;
    std::wstring diagnosticMessage = pipeServer_.LastError();

    if (enabled_ && nativeHostRegistered_ && pipeServer_.IsRunning()) {
        if (pipeServer_.ConnectedClientCount() == 0) {
            ++noClientPollCount_;
            reconnectAttempts_ = noClientPollCount_;
            std::wostringstream output;
            output
                << L"waiting for extension native host client, attempt="
                << noClientPollCount_
                << L"/"
                << kMaxNoClientPollsBeforeError;
            diagnosticMessage = output.str();

            if (noClientPollCount_ >= kMaxNoClientPollsBeforeError) {
                health = WebMonitorHealth::Error;
                WebDebugLog::Write(L"WebSource", L"auto_connect_failed " + diagnosticMessage);
            }
        } else {
            if (noClientPollCount_ > 0) {
                WebDebugLog::Write(L"WebSource", L"auto_connect_recovered");
            }
            noClientPollCount_ = 0;
            reconnectAttempts_ = 0;
        }
    }

    RefreshStateLocked(health, diagnosticMessage);
}

WebAccountState WebSourceController::CurrentState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return currentState_;
}

std::wstring WebSourceController::Diagnostics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return diagnostics_.Build(currentState_, enabled_);
}

std::string WebSourceController::HandleBridgeMessage(const std::string& message)
{
    JsonValue root;
    std::string parseError;
    JsonParser parser;
    if (!parser.Parse(message, &root, &parseError) || !root.IsObject()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++protocolErrorCount_;
        WebDebugLog::WriteUtf8(L"WebSource", "invalid json error=" + parseError);
        RefreshStateLocked(WebMonitorHealth::Degraded, NativeHostProtocol::Utf8ToWide(parseError));
        return NativeHostProtocol::MakeProtocolError("invalid_json");
    }

    return HandleParsedMessage(root);
}

std::string WebSourceController::HandleParsedMessage(const JsonValue& root)
{
    const JsonValue* typeField = root.GetObjectField("type");
    if (typeField == nullptr || !typeField->IsString()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++protocolErrorCount_;
        WebDebugLog::Write(L"WebSource", L"missing message type");
        RefreshStateLocked(WebMonitorHealth::Degraded, L"missing message type");
        return NativeHostProtocol::MakeProtocolError("missing_type");
    }

    const std::string type = typeField->stringValue;
    WebDebugLog::WriteUtf8(L"WebSource", "handle message type=" + type);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        noClientPollCount_ = 0;
        reconnectAttempts_ = 0;
    }
    if (type == "ping" || type == "extension_hello" || type == "tab_registered") {
        std::lock_guard<std::mutex> lock(mutex_);
        RefreshStateLocked(WebMonitorHealth::Normal, L"");
        return type == "ping" ? Pong() : Ack();
    }
    if (type == "snapshot_begin") {
        std::lock_guard<std::mutex> lock(mutex_);
        store_.ClearAll();
        observersByTab_.clear();
        lastSequenceByObserver_.clear();
        RefreshStateLocked(WebMonitorHealth::Normal, L"");
        return Ack();
    }
    if (type == "snapshot_end") {
        std::lock_guard<std::mutex> lock(mutex_);
        RefreshStateLocked(WebMonitorHealth::Normal, L"");
        return Ack();
    }
    if (type == "tab_state") {
        return ApplyTabState(root);
    }
    if (type == "tab_removed") {
        return ApplyTabRemoved(root);
    }
    if (type == "tab_suspended" || type == "tab_lifecycle") {
        return ApplyTabSuspended(root);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ++protocolErrorCount_;
    WebDebugLog::WriteUtf8(L"WebSource", "unknown message type=" + type);
    RefreshStateLocked(WebMonitorHealth::Degraded, NativeHostProtocol::Utf8ToWide("unknown message type: " + type));
    return NativeHostProtocol::MakeProtocolError("unknown_type");
}

std::string WebSourceController::ApplyTabState(const JsonValue& root)
{
    std::string browserInstanceId;
    std::string documentId;
    std::string stateText;
    int tabId = 0;
    int windowId = 0;
    if (!ReadRequiredString(root, "browserInstanceId", &browserInstanceId) ||
        !ReadRequiredString(root, "documentId", &documentId) ||
        !ReadRequiredString(root, "state", &stateText) ||
        !ReadRequiredInt(root, "tabId", &tabId)) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++protocolErrorCount_;
        WebDebugLog::Write(L"WebSource", L"invalid tab_state missing required field");
        RefreshStateLocked(WebMonitorHealth::Degraded, L"invalid tab_state message");
        return NativeHostProtocol::MakeProtocolError("invalid_tab_state");
    }
    ReadRequiredInt(root, "windowId", &windowId);

    WebConversationIdentity identity;
    const JsonValue* conversation = root.GetObjectField("conversation");
    if (conversation != nullptr && conversation->IsObject()) {
        const JsonValue* kind = conversation->GetObjectField("kind");
        const JsonValue* id = conversation->GetObjectField("id");
        identity.kind = kind != nullptr && kind->IsString() ? kind->stringValue : "temporary";
        identity.id = id != nullptr && id->IsString() ? id->stringValue : "";
    }

    uint64_t sequence = 0;
    ReadOptionalUint64(root, "sequence", &sequence);

    std::string reason;
    const JsonValue* reasonField = root.GetObjectField("reason");
    if (reasonField != nullptr && reasonField->IsString()) {
        reason = reasonField->stringValue;
    }

    int64_t observedAt = CurrentTimeMs();
    const JsonValue* observedAtField = root.GetObjectField("observedAt");
    if (observedAtField != nullptr && observedAtField->IsNumber()) {
        observedAt = static_cast<int64_t>(observedAtField->numberValue);
    }

    ChatGptConversationStore keyBuilder;
    PageObserverRecord record;
    record.browserInstanceId = browserInstanceId;
    record.tabId = tabId;
    record.windowId = windowId;
    record.documentId = documentId;
    record.observerId = keyBuilder.BuildObserverId(browserInstanceId, tabId, documentId);
    record.conversationKey = keyBuilder.BuildConversationKey(browserInstanceId, identity, tabId, documentId);
    record.state = ParseObservedState(stateText);
    record.reason = reason;
    record.lastObservedAt = observedAt;
    record.lastStrongSignalAt =
        record.state == WebObservedPageState::Running || record.state == WebObservedPageState::WaitingInput
        ? observedAt
        : 0;
    record.visible = true;
    record.focused = false;
    record.observerHealthy = true;

    std::lock_guard<std::mutex> lock(mutex_);
    if (sequence > 0) {
        const uint64_t lastSequence = lastSequenceByObserver_[record.observerId];
        if (sequence <= lastSequence) {
            WebDebugLog::WriteUtf8(L"WebSource", "ignore stale tab_state state=" + stateText + " reason=" + reason);
            return Ack();
        }
        lastSequenceByObserver_[record.observerId] = sequence;
    }

    browserInstances_.insert(browserInstanceId);
    observersByTab_[TabKey(browserInstanceId, tabId)].insert(record.observerId);
    store_.ApplyObservation(record);
    RefreshStateLocked(WebMonitorHealth::Normal, L"");
    std::wostringstream output;
    output
        << L"apply tab_state state=" << NativeHostProtocol::Utf8ToWide(stateText)
        << L" reason=" << NativeHostProtocol::Utf8ToWide(reason)
        << L" tab_id=" << tabId
        << L" running=" << currentState_.runningCount
        << L" waiting=" << currentState_.waitingCount
        << L" completed=" << currentState_.completedCount;
    WebDebugLog::Write(L"WebSource", output.str());
    return Ack();
}

std::string WebSourceController::ApplyTabRemoved(const JsonValue& root)
{
    std::string browserInstanceId;
    int tabId = 0;
    if (!ReadRequiredString(root, "browserInstanceId", &browserInstanceId) ||
        !ReadRequiredInt(root, "tabId", &tabId)) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++protocolErrorCount_;
        WebDebugLog::Write(L"WebSource", L"invalid tab_removed missing required field");
        RefreshStateLocked(WebMonitorHealth::Degraded, L"invalid tab_removed message");
        return NativeHostProtocol::MakeProtocolError("invalid_tab_removed");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string tabKey = TabKey(browserInstanceId, tabId);
    auto observers = observersByTab_.find(tabKey);
    if (observers != observersByTab_.end()) {
        for (const std::string& observerId : observers->second) {
            store_.RemoveObserver(observerId);
            lastSequenceByObserver_.erase(observerId);
        }
        observersByTab_.erase(observers);
    }
    RefreshStateLocked(WebMonitorHealth::Normal, L"");
    WebDebugLog::Write(L"WebSource", WideCount(L"remove tab tab_id=", tabId));
    return Ack();
}

std::string WebSourceController::ApplyTabSuspended(const JsonValue& root)
{
    std::string browserInstanceId;
    std::string documentId;
    int tabId = 0;
    if (!ReadRequiredString(root, "browserInstanceId", &browserInstanceId) ||
        !ReadRequiredString(root, "documentId", &documentId) ||
        !ReadRequiredInt(root, "tabId", &tabId)) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++protocolErrorCount_;
        WebDebugLog::Write(L"WebSource", L"invalid tab_suspended missing required field");
        RefreshStateLocked(WebMonitorHealth::Degraded, L"invalid tab_suspended message");
        return NativeHostProtocol::MakeProtocolError("invalid_tab_suspended");
    }

    ChatGptConversationStore keyBuilder;
    const std::string observerId = keyBuilder.BuildObserverId(browserInstanceId, tabId, documentId);

    std::lock_guard<std::mutex> lock(mutex_);
    store_.RemoveObserver(observerId);
    lastSequenceByObserver_.erase(observerId);
    RefreshStateLocked(WebMonitorHealth::Degraded, L"tab suspended or discarded");
    WebDebugLog::Write(L"WebSource", WideCount(L"suspend tab tab_id=", tabId));
    return Ack();
}

void WebSourceController::RefreshStateLocked(WebMonitorHealth health, const std::wstring& diagnosticMessage)
{
    currentState_ = aggregator_.Aggregate(
        store_.Conversations(),
        browserInstances_.size(),
        store_.ObserverCount(),
        store_.ObserverCount(),
        store_.SuspendedObserverCount(),
        health);
    currentState_.nativeBridgeClients = pipeServer_.ConnectedClientCount();
    currentState_.nativeHostRegistered = nativeHostRegistered_;
    currentState_.protocolErrorCount = protocolErrorCount_;
    currentState_.nativeReconnectAttempts = reconnectAttempts_;
    currentState_.lastReason = store_.LastReason();
    currentState_.lastStateChangedAt = store_.LastStateChangedAt();
    currentState_.bridgeState = enabled_
        ? (pipeServer_.IsRunning() ? "listening" : "stopped")
        : "disabled";
    currentState_.diagnosticMessage = diagnosticMessage;
}

bool WebSourceController::ReadRequiredString(const JsonValue& root, const std::string& name, std::string* value)
{
    const JsonValue* field = root.GetObjectField(name);
    if (field == nullptr || !field->IsString() || field->stringValue.empty() || field->stringValue.size() > 256) {
        return false;
    }
    *value = field->stringValue;
    return true;
}

bool WebSourceController::ReadRequiredInt(const JsonValue& root, const std::string& name, int* value)
{
    const JsonValue* field = root.GetObjectField(name);
    if (field == nullptr || !field->IsNumber()) {
        return false;
    }
    *value = static_cast<int>(field->numberValue);
    return true;
}

bool WebSourceController::ReadOptionalUint64(const JsonValue& root, const std::string& name, uint64_t* value)
{
    const JsonValue* field = root.GetObjectField(name);
    if (field == nullptr) {
        return false;
    }
    if (!field->IsNumber() || field->numberValue < 0) {
        return false;
    }
    *value = static_cast<uint64_t>(field->numberValue);
    return true;
}

WebObservedPageState WebSourceController::ParseObservedState(const std::string& state) const
{
    if (state == "idle") {
        return WebObservedPageState::Idle;
    }
    if (state == "running") {
        return WebObservedPageState::Running;
    }
    if (state == "waiting") {
        return WebObservedPageState::WaitingInput;
    }
    if (state == "terminal_success") {
        return WebObservedPageState::TerminalSuccess;
    }
    if (state == "terminal_failed") {
        return WebObservedPageState::TerminalFailed;
    }
    if (state == "terminal_cancelled") {
        return WebObservedPageState::TerminalCancelled;
    }
    return WebObservedPageState::Unknown;
}

std::string WebSourceController::TabKey(const std::string& browserInstanceId, int tabId) const
{
    return browserInstanceId + ":tab:" + std::to_string(tabId);
}
