/**
 * 文件作用：实现 ChatGPT Web 监听阶段自检命令
 * 职责范围：
 * 1. 验证同 conversationId 多标签页只计一个任务
 * 2. 验证等待、运行、完成三色优先级和终止去重
 * 3. 验证失效观察器不会保留旧运行/等待贡献
 *
 * 不负责：
 * - 模拟完整 Chrome 扩展运行时
 * - 验收具体 ChatGPT 页面 DOM 选择器
 *
 * 维护说明：
 * - 每新增一个影响计数的规则，都应补充这里的内存级断言
 */
#include "WebSelfTest.h"

#include "ChatGptAccountAggregator.h"
#include "ChatGptConversationStore.h"
#include "NativeHostProtocol.h"
#include "NativeHostRegistration.h"
#include "WebSourceController.h"

#include <iostream>
#include <set>

namespace {

PageObserverRecord MakeObserver(
    const std::string& observerId,
    const std::string& conversationKey,
    WebObservedPageState state,
    int64_t observedAt,
    const std::string& reason)
{
    PageObserverRecord record;
    record.observerId = observerId;
    record.conversationKey = conversationKey;
    record.state = state;
    record.lastObservedAt = observedAt;
    record.lastStrongSignalAt =
        state == WebObservedPageState::Running || state == WebObservedPageState::WaitingInput
        ? observedAt
        : 0;
    record.reason = reason;
    record.observerHealthy = true;
    record.visible = true;
    return record;
}

bool Expect(bool value, const char* name)
{
    std::cout << (value ? "PASS " : "FAIL ") << name << "\n";
    return value;
}

bool VerifyPipePingPong()
{
    WebSourceController controller;
    controller.Enable();
    Sleep(100);

    HANDLE pipe = CreateFileW(
        NativeHostProtocol::WebBridgePipeName().c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        controller.Disable();
        return false;
    }

    std::wstring errorMessage;
    const bool sent = NativeHostProtocol::WriteFramedMessage(
        pipe,
        "{\"protocolVersion\":1,\"type\":\"ping\"}",
        &errorMessage);
    std::string response;
    const bool received = sent && NativeHostProtocol::ReadFramedMessage(pipe, &response, &errorMessage);
    CloseHandle(pipe);
    controller.Disable();

    return received && response.find("\"type\":\"pong\"") != std::string::npos;
}

bool VerifyNativeHostRegistration()
{
    NativeHostRegistration registration;
    std::wstring errorMessage;
    if (!registration.EnsureRegistered(&errorMessage)) {
        return false;
    }

    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.statuslight.web",
        0,
        KEY_READ,
        &key);
    if (openResult != ERROR_SUCCESS) {
        return false;
    }

    wchar_t registryValue[MAX_PATH] {};
    DWORD registryValueSize = sizeof(registryValue);
    const LONG readResult = RegQueryValueExW(
        key,
        nullptr,
        nullptr,
        nullptr,
        reinterpret_cast<LPBYTE>(registryValue),
        &registryValueSize);
    RegCloseKey(key);

    if (readResult != ERROR_SUCCESS || registryValue[0] == L'\0') {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(registryValue);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }

    return true;
}

} // namespace

int WebSelfTest::Run()
{
    bool ok = true;
    ok = Expect(VerifyNativeHostRegistration(), "P0 Native Host manifest and HKCU registration exist") && ok;
    ok = Expect(VerifyPipePingPong(), "P0 Native Messaging pipe ping/pong works") && ok;
    const std::string browserInstanceId = "browser-test";
    WebConversationIdentity conversationOne { "persistent", "conversation-1" };
    WebConversationIdentity conversationTwo { "persistent", "conversation-2" };
    WebConversationIdentity conversationThree { "persistent", "conversation-3" };
    WebConversationIdentity temporaryConversation { "temporary", "" };

    ChatGptConversationStore store;
    const std::string keyA = store.BuildConversationKey(browserInstanceId, conversationOne, 1, "doc-a");
    const std::string keyB = store.BuildConversationKey(browserInstanceId, conversationOne, 2, "doc-b");
    const std::string keyC = store.BuildConversationKey(browserInstanceId, conversationTwo, 3, "doc-c");
    ok = Expect(keyA == keyB && keyA != keyC, "P4 conversationKey deduplicates duplicate tabs") && ok;

    store.ApplyObservation(MakeObserver("observer-a", keyA, WebObservedPageState::Running, 1000, "visible-stop-control"));
    store.ApplyObservation(MakeObserver("observer-b", keyB, WebObservedPageState::Running, 1100, "visible-stop-control"));
    store.ApplyObservation(MakeObserver("observer-c", keyC, WebObservedPageState::WaitingInput, 1200, "explicit-human-gate"));

    ChatGptAccountAggregator aggregator;
    WebAccountState state = aggregator.Aggregate(store.Conversations(), 1, 3, store.ObserverCount(), 0, WebMonitorHealth::Normal);
    ok = Expect(state.runningCount == 1, "P3/P4 duplicate running tabs count once") && ok;
    ok = Expect(state.waitingCount == 1, "P3 waiting input is counted") && ok;
    ok = Expect(state.duplicateObservers == 1, "P4 duplicate observer count is diagnostic only") && ok;

    store.ApplyObservation(MakeObserver("observer-a", keyA, WebObservedPageState::Idle, 4200, "stable-idle-after-active"));
    store.ApplyObservation(MakeObserver("observer-b", keyB, WebObservedPageState::Idle, 4300, "stable-idle-after-active"));
    state = aggregator.Aggregate(store.Conversations(), 1, 3, store.ObserverCount(), 0, WebMonitorHealth::Normal);
    ok = Expect(state.completedCount == 1, "P4 terminal event is deduplicated by operationGeneration") && ok;
    ok = Expect(state.waitingCount == 1, "P5 waiting keeps priority over completed") && ok;

    store.RemoveMissingObservers(std::set<std::string> { "observer-c" });
    state = aggregator.Aggregate(store.Conversations(), 1, 1, store.ObserverCount(), 0, WebMonitorHealth::Degraded);
    ok = Expect(state.runningCount == 0 && state.waitingCount == 1, "P6 stale duplicate observers do not keep running") && ok;
    ok = Expect(state.health == WebMonitorHealth::Degraded, "P6 degraded health is preserved") && ok;

    ChatGptConversationStore migrationStore;
    const std::string temporaryKey = migrationStore.BuildConversationKey(browserInstanceId, temporaryConversation, 4, "doc-temp");
    migrationStore.ApplyObservation(MakeObserver("observer-temp", temporaryKey, WebObservedPageState::Running, 1000, "visible-stop-control"));
    const std::string promotedKey = migrationStore.BuildConversationKey(browserInstanceId, conversationThree, 4, "doc-temp");
    migrationStore.ApplyObservation(MakeObserver("observer-temp", promotedKey, WebObservedPageState::Running, 1500, "visible-stop-control"));
    std::vector<WebConversationRecord> migrated = migrationStore.Conversations();
    ok = Expect(migrated.size() == 1 && migrated.front().operationGeneration == 1, "P4 temporary key migrates without creating a new task") && ok;

    PageObserverRecord suspended = MakeObserver("observer-temp", promotedKey, WebObservedPageState::Running, 2000, "visible-stop-control");
    suspended.suspended = true;
    migrationStore.ApplyObservation(suspended);
    state = aggregator.Aggregate(migrationStore.Conversations(), 1, 1, migrationStore.ObserverCount(), 1, WebMonitorHealth::Degraded);
    ok = Expect(state.runningCount == 0, "P6 suspended observer removes active contribution") && ok;

    ChatGptConversationStore ownerCloseStore;
    const std::string ownerKey = ownerCloseStore.BuildConversationKey(browserInstanceId, conversationOne, 1, "doc-a");
    ownerCloseStore.ApplyObservation(MakeObserver("owner-tab", ownerKey, WebObservedPageState::Running, 1000, "visible-stop-control"));
    PageObserverRecord backup = MakeObserver("backup-tab", ownerKey, WebObservedPageState::Running, 1100, "visible-stop-control");
    backup.suspended = true;
    ownerCloseStore.ApplyObservation(backup);
    ownerCloseStore.RemoveMissingObservers(std::set<std::string> { "backup-tab" });
    state = aggregator.Aggregate(ownerCloseStore.Conversations(), 1, 1, ownerCloseStore.ObserverCount(), 1, WebMonitorHealth::Degraded);
    ok = Expect(state.runningCount == 0, "P6 closing owner with only frozen backup clears running contribution") && ok;

    ChatGptConversationStore cancelStore;
    const std::string cancelKey = cancelStore.BuildConversationKey(browserInstanceId, conversationOne, 1, "doc-a");
    cancelStore.ApplyObservation(MakeObserver("cancel-tab", cancelKey, WebObservedPageState::Running, 1000, "visible-stop-control"));
    cancelStore.ApplyObservation(MakeObserver("cancel-tab", cancelKey, WebObservedPageState::TerminalCancelled, 1600, "stop-control-clicked"));
    state = aggregator.Aggregate(cancelStore.Conversations(), 1, 1, cancelStore.ObserverCount(), 0, WebMonitorHealth::Normal);
    ok = Expect(
        state.completedCount == 1 && state.runningCount == 0 && state.waitingCount == 0,
        "P3 cancelled terminal state counts as completed only") && ok;

    std::cout << "P0 self-check: Native Messaging data model and bridge aggregation compiled\n";
    std::cout << "P1 self-check: extension observer identity model compiled\n";
    std::cout << "P3 self-check: state classification invariants verified\n";
    std::cout << "P4 self-check: multi-tab dedupe invariants verified\n";
    std::cout << "P5 self-check: account/global count inputs verified\n";
    std::cout << "P6 self-check: invalid observer cleanup invariant verified\n";
    return ok ? 0 : 1;
}
