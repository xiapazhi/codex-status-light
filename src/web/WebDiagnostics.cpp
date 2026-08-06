/**
 * 文件作用：实现 ChatGPT Web 监听诊断文本生成
 * 职责范围：
 * 1. 汇总 Web 扩展桥接连接和聚合状态
 * 2. 输出每个逻辑对话的短 ID、Owner、观察器数量和状态
 *
 * 不负责：
 * - 暴露聊天正文、标题或用户账号信息
 * - 判定业务状态
 *
 * 维护说明：
 * - 新增诊断字段时先确认不会泄露网页内容或认证信息
 */
#include "WebDiagnostics.h"

#include <Windows.h>

#include <sstream>

std::wstring WebDiagnostics::Build(const WebAccountState& state, bool enabled) const
{
    std::wostringstream output;
    output << L"Web monitoring: " << (enabled ? L"enabled" : L"disabled") << L"\n";
    output << L"Web extension installed: unknown\n";
    output << L"Extension protocol: 1\n";
    output << L"Native host registered: " << (state.nativeHostRegistered ? L"yes" : L"no") << L"\n";
    output << L"Native bridge state: " << Widen(state.bridgeState.empty() ? "unknown" : state.bridgeState) << L"\n";
    output << L"Native bridge clients: " << state.nativeBridgeClients << L"\n";
    output << L"Browser profiles: " << state.chromeProfileScopes << L"\n";
    output << L"ChatGPT tabs: " << state.chatGptTabs << L"\n";
    output << L"Web conversations: " << state.conversations.size() << L"\n";
    output << L"Duplicate observers: " << state.duplicateObservers << L"\n";
    output << L"Active page observers: " << state.activePageObservers << L"\n";
    output << L"Suspended observers: " << state.suspendedObservers << L"\n";
    output << L"Web waiting: " << state.waitingCount << L"\n";
    output << L"Web running: " << state.runningCount << L"\n";
    output << L"Web terminal: " << state.completedCount << L"\n";
    output << L"Last web reason: " << Widen(state.lastReason.empty() ? "unknown" : state.lastReason) << L"\n";
    output << L"Last web state age: " << (state.lastStateChangedAt == 0 ? L"unknown" : L"available") << L"\n";
    output << L"Last active snapshot request: "
        << (state.lastActiveSnapshotRequestAt == 0 ? L"never" : L"available")
        << L"\n";
    output << L"Last active snapshot result: "
        << Widen(state.lastActiveSnapshotResult.empty() ? "none" : state.lastActiveSnapshotResult)
        << L"\n";
    output << L"Native reconnect attempts: " << state.nativeReconnectAttempts << L"\n";
    output << L"Protocol errors: " << state.protocolErrorCount << L"\n";
    if (!state.diagnosticMessage.empty()) {
        output << L"Web diagnostic: " << state.diagnosticMessage << L"\n";
    }

    for (const WebConversationRecord& conversation : state.conversations) {
        output << L"Conversation " << Widen(ShortId(conversation.conversationKey))
            << L" Owner " << Widen(ShortId(conversation.activeOwnerObserverId))
            << L" Observers " << conversation.observerIds.size()
            << L" State " << StateText(conversation.state)
            << L" Generation " << conversation.operationGeneration;
        if (conversation.terminalReason.has_value()) {
            output << L" Terminal " << Widen(conversation.terminalReason->reason);
        }
        output << L"\n";
    }

    return output.str();
}

std::wstring WebDiagnostics::Widen(const std::string& value) const
{
    if (value.empty()) {
        return std::wstring();
    }
    const int requiredSize = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (requiredSize <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<size_t>(requiredSize), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), requiredSize);
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}

std::wstring WebDiagnostics::StateText(WebConversationState state) const
{
    switch (state) {
    case WebConversationState::Idle:
        return L"Idle";
    case WebConversationState::Running:
        return L"Running";
    case WebConversationState::WaitingInput:
        return L"WaitingInput";
    case WebConversationState::TerminalSuccess:
        return L"TerminalSuccess";
    case WebConversationState::TerminalFailed:
        return L"TerminalFailed";
    case WebConversationState::TerminalCancelled:
        return L"TerminalCancelled";
    case WebConversationState::Unknown:
    default:
        return L"Unknown";
    }
}

std::string WebDiagnostics::ShortId(const std::string& value) const
{
    if (value.size() <= 8) {
        return value;
    }
    return value.substr(value.size() - 8);
}
