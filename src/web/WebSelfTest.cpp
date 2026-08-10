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
#include "../celebration/CelebrationController.h"

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
    record.browserInstanceId = "browser-test";
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

AggregateTransition MakeCelebrationTransition(
    CelebrationVisualState previous,
    CelebrationVisualState current,
    uint32_t newSuccessCount,
    uint32_t newFailedCount,
    uint32_t newCancelledCount,
    uint32_t waitingCount,
    uint32_t runningCount)
{
    AggregateTransition transition;
    transition.previousVisual = previous;
    transition.currentVisual = current;
    transition.newSuccessCount = newSuccessCount;
    transition.newFailedCount = newFailedCount;
    transition.newCancelledCount = newCancelledCount;
    transition.waitingCount = waitingCount;
    transition.runningCount = runningCount;
    transition.completedCount = newSuccessCount + newFailedCount + newCancelledCount;
    return transition;
}

bool VerifyCelebrationTriggerRules()
{
    bool pending = false;
    bool ok = true;

    AggregateTransition success = MakeCelebrationTransition(
        CelebrationVisualState::Running,
        CelebrationVisualState::Completed,
        1,
        0,
        0,
        0,
        0);
    ok = Expect(CelebrationController::UpdatePendingForTransition(&pending, success) && !pending, "P7 success completion triggers celebration") && ok;

    pending = false;
    AggregateTransition failed = MakeCelebrationTransition(
        CelebrationVisualState::Running,
        CelebrationVisualState::Completed,
        0,
        1,
        0,
        0,
        0);
    ok = Expect(!CelebrationController::UpdatePendingForTransition(&pending, failed) && !pending, "P7 failed completion does not trigger celebration") && ok;

    pending = false;
    AggregateTransition cancelled = MakeCelebrationTransition(
        CelebrationVisualState::Running,
        CelebrationVisualState::Completed,
        0,
        0,
        1,
        0,
        0);
    ok = Expect(!CelebrationController::UpdatePendingForTransition(&pending, cancelled) && !pending, "P7 cancelled completion does not trigger celebration") && ok;

    pending = false;
    AggregateTransition partialSuccess = MakeCelebrationTransition(
        CelebrationVisualState::Running,
        CelebrationVisualState::Running,
        1,
        0,
        0,
        0,
        1);
    ok = Expect(CelebrationController::UpdatePendingForTransition(&pending, partialSuccess) && !pending, "P7 partial success triggers while running remains") && ok;

    AggregateTransition finalCompletion = MakeCelebrationTransition(
        CelebrationVisualState::Running,
        CelebrationVisualState::Completed,
        0,
        0,
        0,
        0,
        0);
    ok = Expect(!CelebrationController::UpdatePendingForTransition(&pending, finalCompletion) && !pending, "P7 aggregate completion without new success does not retrigger") && ok;

    pending = false;
    AggregateTransition fastSuccess = MakeCelebrationTransition(
        CelebrationVisualState::Completed,
        CelebrationVisualState::Completed,
        1,
        0,
        0,
        0,
        0);
    ok = Expect(CelebrationController::UpdatePendingForTransition(&pending, fastSuccess) && !pending, "P7 fast success at rest triggers after baseline") && ok;

    return ok;
}

} // namespace

int WebSelfTest::Run()
{
    bool ok = true;
    ok = Expect(VerifyNativeHostRegistration(), "P0 Native Host manifest and HKCU registration exist") && ok;
    ok = Expect(VerifyPipePingPong(), "P0 Native Messaging pipe ping/pong works") && ok;
    ok = VerifyCelebrationTriggerRules() && ok;
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

    store.ApplyObservation(MakeObserver("observer-a", keyA, WebObservedPageState::Idle, 1200, "no_active_signal"));
    store.ApplyObservation(MakeObserver("observer-b", keyB, WebObservedPageState::Idle, 1300, "no_active_signal"));
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

    ChatGptConversationStore editStore;
    const std::string editKey = editStore.BuildConversationKey(browserInstanceId, conversationOne, 1, "doc-edit");
    editStore.ApplyObservation(MakeObserver("edit-tab", editKey, WebObservedPageState::WaitingInput, 1000, "explicit-gate"));
    editStore.ApplyObservation(MakeObserver("edit-tab", editKey, WebObservedPageState::Idle, 3200, "edit-idle-before-send"));
    state = aggregator.Aggregate(editStore.Conversations(), 1, 1, editStore.ObserverCount(), 0, WebMonitorHealth::Normal);
    ok = Expect(
        state.completedCount == 0 && state.runningCount == 0 && state.waitingCount == 0,
        "P7 edit-only idle does not create successful completion") && ok;

    editStore.ApplyObservation(MakeObserver("edit-tab", editKey, WebObservedPageState::Running, 4000, "visible-stop-control"));
    editStore.ApplyObservation(MakeObserver("edit-tab", editKey, WebObservedPageState::Idle, 4100, "no_active_signal"));
    state = aggregator.Aggregate(editStore.Conversations(), 1, 1, editStore.ObserverCount(), 0, WebMonitorHealth::Normal);
    ok = Expect(state.completedCount == 1, "P7 running then idle immediately creates successful completion") && ok;

    ChatGptConversationStore snapshotCleanupStore;
    const std::string staleKey = snapshotCleanupStore.BuildConversationKey(browserInstanceId, conversationOne, 8, "doc-stale");
    const std::string currentKey = snapshotCleanupStore.BuildConversationKey(browserInstanceId, conversationTwo, 9, "doc-current");
    snapshotCleanupStore.ApplyObservation(MakeObserver("stale-running-tab", staleKey, WebObservedPageState::Running, 1000, "visible-stop-control"));
    snapshotCleanupStore.ApplyObservation(MakeObserver("current-idle-tab", currentKey, WebObservedPageState::Idle, 5000, "snapshot-idle"));
    snapshotCleanupStore.RemoveMissingObservers(std::set<std::string> { "current-idle-tab" });
    state = aggregator.Aggregate(snapshotCleanupStore.Conversations(), 1, 1, snapshotCleanupStore.ObserverCount(), 0, WebMonitorHealth::Normal);
    ok = Expect(
        state.runningCount == 0 && state.waitingCount == 0,
        "P8 active snapshot cleanup removes stale running observer") && ok;

    ChatGptConversationStore browserSnapshotStore;
    const std::string sameBrowserOldKey = browserSnapshotStore.BuildConversationKey(
        browserInstanceId,
        conversationOne,
        10,
        "doc-old");
    const std::string sameBrowserCurrentKey = browserSnapshotStore.BuildConversationKey(
        browserInstanceId,
        conversationTwo,
        11,
        "doc-current");
    browserSnapshotStore.ApplyObservation(MakeObserver(
        "same-browser-old-running",
        sameBrowserOldKey,
        WebObservedPageState::Running,
        1000,
        "visible-stop-control"));
    browserSnapshotStore.ApplyObservation(MakeObserver(
        "same-browser-current-idle",
        sameBrowserCurrentKey,
        WebObservedPageState::Idle,
        5000,
        "snapshot-idle"));

    PageObserverRecord otherBrowser = MakeObserver(
        "other-browser-running",
        "browser-other:conversation:conversation-9",
        WebObservedPageState::Running,
        1200,
        "visible-stop-control");
    otherBrowser.browserInstanceId = "browser-other";
    browserSnapshotStore.ApplyObservation(otherBrowser);

    const size_t removedFromBrowser = browserSnapshotStore.RemoveMissingObserversForBrowser(
        browserInstanceId,
        std::set<std::string> { "same-browser-current-idle" });
    state = aggregator.Aggregate(
        browserSnapshotStore.Conversations(),
        2,
        2,
        browserSnapshotStore.ObserverCount(),
        0,
        WebMonitorHealth::Normal);
    ok = Expect(removedFromBrowser == 1, "P8 browser snapshot reports removed stale observer") && ok;
    ok = Expect(state.runningCount == 1, "P8 browser snapshot keeps other browser observers") && ok;

    std::cout << "P0 self-check: Native Messaging data model and bridge aggregation compiled\n";
    std::cout << "P1 self-check: extension observer identity model compiled\n";
    std::cout << "P3 self-check: state classification invariants verified\n";
    std::cout << "P4 self-check: multi-tab dedupe invariants verified\n";
    std::cout << "P5 self-check: account/global count inputs verified\n";
    std::cout << "P6 self-check: invalid observer cleanup invariant verified\n";
    std::cout << "P7 self-check: celebration trigger and deduplication invariants verified\n";
    return ok ? 0 : 1;
}
