/**
 * 文件作用：实现 P1 控制台状态命令
 * 职责范围：
 * 1. 调用 StatusReader 获取状态快照
 * 2. 输出会话状态、统计计数和额度摘要
 * 3. 在 watch 模式下定期刷新状态
 *
 * 不负责：
 * - JSONL 事件映射和状态机逻辑
 * - 托盘图标显示
 *
 * 维护说明：
 * - 输出内容必须保持简洁，详细事件取证继续使用 --inspect
 */
#include "StatusCommand.h"

#include "AppVersion.h"

#include <chrono>
#include <iostream>
#include <thread>

int StatusCommand::Run(const StatusOptions& options)
{
    StatusReadOptions readOptions;
    readOptions.codexHome = options.codexHome;
    readOptions.maxFiles = options.maxFiles;
    readOptions.recentHours = options.recentHours;

    StatusReader reader;
    StatusSnapshot snapshot = reader.ReadOnce(readOptions);
    PrintStatus(snapshot);

    if (snapshot.hasSourceError) {
        return 2;
    }
    if (!options.watch) {
        return snapshot.parseErrorCount > 0 ? 1 : 0;
    }

    std::cout << "\nWatching status. Press Ctrl+C to stop.\n";
    std::cout.flush();
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(options.pollSeconds));
        snapshot = reader.ReadOnce(readOptions);
        PrintStatus(snapshot);
    }
}

void StatusCommand::PrintStatus(const StatusSnapshot& snapshot) const
{
    size_t runningCount = 0;
    size_t waitingCount = 0;
    size_t completedCount = 0;
    size_t failedCount = 0;
    size_t cancelledCount = 0;
    size_t unknownCount = 0;
    size_t staleCount = 0;

    std::cout << "\nStatus summary\n";
    std::cout << "Version: " << AppVersion::kStatusLightVersion << "\n";
    if (snapshot.hasSourceError) {
        std::wcout << L"Source error: " << snapshot.errorMessage << L"\n";
    }
    std::cout << "Parsed lines: " << snapshot.parsedLineCount
        << ", JSON errors: " << snapshot.parseErrorCount
        << ", Unknown events: " << snapshot.unknownEventCount << "\n";

    for (const auto& item : snapshot.sessions) {
        const SessionState& session = item.second;
        switch (session.state) {
        case TaskState::Running:
            runningCount++;
            break;
        case TaskState::WaitingInput:
            waitingCount++;
            break;
        case TaskState::Completed:
            completedCount++;
            break;
        case TaskState::Failed:
            failedCount++;
            break;
        case TaskState::Cancelled:
            cancelledCount++;
            break;
        case TaskState::Stale:
            staleCount++;
            break;
        case TaskState::Unknown:
            unknownCount++;
            break;
        }

        std::cout << "Session " << session.sessionId
            << ": " << TaskStateToString(session.state);
        if (!session.taskId.empty()) {
            std::cout << " turn=" << session.taskId;
        }
        if (!session.lastRawEvent.empty()) {
            std::cout << " last=" << session.lastRawEvent;
        }
        if (!session.lastEventTime.empty()) {
            std::cout << " at=" << session.lastEventTime;
        }
        std::cout << "\n";
    }

    std::cout << "Counts: waiting=" << waitingCount
        << ", running=" << runningCount
        << ", completed=" << completedCount
        << ", failed=" << failedCount
        << ", cancelled=" << cancelledCount
        << ", stale=" << staleCount
        << ", unknown=" << unknownCount << "\n";

    std::cout << "Quota: " << QuotaValidityToString(snapshot.quota.validity);
    if (snapshot.quota.validity != QuotaValidity::Unavailable) {
        std::cout << ", effective remaining=" << snapshot.quota.effectiveRemaining << "%";
    }
    if (snapshot.quota.primary.has_value()) {
        std::cout << ", primary remaining=" << snapshot.quota.primary->remainingPercent << "%";
    }
    if (snapshot.quota.secondary.has_value()) {
        std::cout << ", secondary remaining=" << snapshot.quota.secondary->remainingPercent << "%";
    }
    if (!snapshot.quota.planType.empty()) {
        std::cout << ", plan=" << snapshot.quota.planType;
    }
    if (!snapshot.quota.receivedAt.empty()) {
        std::cout << ", received=" << snapshot.quota.receivedAt;
    }
    std::cout << "\n";
    std::cout.flush();
}

std::string StatusCommand::TaskStateToString(TaskState state) const
{
    switch (state) {
    case TaskState::Running:
        return "RUNNING";
    case TaskState::WaitingInput:
        return "WAITING_INPUT";
    case TaskState::Completed:
        return "COMPLETED";
    case TaskState::Failed:
        return "FAILED";
    case TaskState::Cancelled:
        return "CANCELLED";
    case TaskState::Stale:
        return "STALE";
    case TaskState::Unknown:
    default:
        return "UNKNOWN";
    }
}

std::string StatusCommand::QuotaValidityToString(QuotaValidity validity) const
{
    switch (validity) {
    case QuotaValidity::Valid:
        return "valid";
    case QuotaValidity::Partial:
        return "partial";
    case QuotaValidity::Stale:
        return "stale";
    case QuotaValidity::Ambiguous:
        return "ambiguous";
    case QuotaValidity::Unavailable:
    default:
        return "unavailable";
    }
}
