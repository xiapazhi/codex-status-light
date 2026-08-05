/**
 * 文件作用：提供 Codex JSONL 状态快照读取器
 * 职责范围：
 * 1. 扫描最近 rollout JSONL 文件
 * 2. 基于 P0 事件映射生成会话状态
 * 3. 解析 rate_limits 额度快照
 *
 * 不负责：
 * - 控制台输出格式
 * - 托盘 UI 和图标渲染
 *
 * 维护说明：
 * - 控制台 P1 和托盘 P2 必须共用此读取器，避免状态判断分叉
 */
#pragma once

#include "JsonValue.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct StatusReadOptions {
    std::wstring codexHome;
    size_t maxFiles = 30;
    int recentHours = 24;
    int staleMinutes = 30;
};

enum class TaskState {
    Unknown,
    Running,
    WaitingInput,
    Completed,
    Failed,
    Cancelled,
    Stale
};

enum class QuotaValidity {
    Valid,
    Partial,
    Stale,
    Unavailable,
    Ambiguous
};

struct SessionState {
    std::string sessionId;
    std::string taskId;
    std::wstring sourceFile;
    TaskState state = TaskState::Unknown;
    std::string lastRawEvent;
    std::string lastEventTime;
    int64_t lastEventMs = 0;
    bool sawRecognizedEvent = false;
};

struct QuotaWindow {
    double usedPercent = 0.0;
    int windowMinutes = 0;
    int64_t resetAt = 0;
    double remainingPercent = 0.0;
};

struct QuotaSnapshot {
    std::optional<QuotaWindow> primary;
    std::optional<QuotaWindow> secondary;
    double effectiveRemaining = 0.0;
    std::string planType;
    std::string receivedAt;
    QuotaValidity validity = QuotaValidity::Unavailable;
};

struct StatusSnapshot {
    std::map<std::string, SessionState> sessions;
    QuotaSnapshot quota;
    size_t parsedLineCount = 0;
    size_t parseErrorCount = 0;
    size_t unknownEventCount = 0;
    size_t recognizedEventCount = 0;
    size_t trackedFileCount = 0;
    bool hasSourceError = false;
    std::wstring errorMessage;
    std::wstring codexRoot;
    std::wstring sessionsPath;
};

class StatusReader {
public:
    StatusSnapshot ReadOnce(const StatusReadOptions& options);

private:
    struct RolloutFile {
        std::filesystem::path path;
        std::filesystem::file_time_type lastWriteTime;
        uintmax_t size = 0;
    };

    struct NormalizedEvent {
        std::string type;
        std::string rawEventName;
        std::string sessionId;
        std::string taskId;
        std::string callName;
        std::string timestamp;
        int64_t timestampMs = 0;
        std::wstring sourceFile;
    };

    struct FileCursor {
        std::filesystem::path path;
        uintmax_t offset = 0;
        uintmax_t knownSize = 0;
        std::string incompleteLine;
        uint32_t consecutiveErrors = 0;
    };

    std::vector<RolloutFile> FindRecentRolloutFiles(const std::wstring& sessionsPath, size_t maxFiles, int recentHours);
    void ProcessFile(const RolloutFile& file, StatusSnapshot* snapshot);
    void RemoveSessionsFromFile(const std::filesystem::path& sourceFile, StatusSnapshot* snapshot);
    void ProcessLine(const JsonValue& root, const std::filesystem::path& sourceFile, StatusSnapshot* snapshot);
    NormalizedEvent NormalizeEvent(const JsonValue& root, const std::filesystem::path& sourceFile);
    void ApplyEvent(const NormalizedEvent& event, StatusSnapshot* snapshot);
    void UpdateQuota(const JsonValue& root, const std::string& timestamp, StatusSnapshot* snapshot);
    void ApplyStalePolicy(int staleMinutes, StatusSnapshot* snapshot) const;

    std::string GetTopType(const JsonValue& root) const;
    std::string GetPayloadType(const JsonValue& root) const;
    std::string GetPayloadName(const JsonValue& root) const;
    std::string GetTimestamp(const JsonValue& root) const;
    std::string GetSessionId(const JsonValue& root, const std::filesystem::path& sourceFile) const;
    std::string GetTaskId(const JsonValue& root) const;
    std::string GetNestedString(const JsonValue& root, const std::vector<std::string>& path) const;
    bool HasPayloadField(const JsonValue& root, const std::string& fieldName) const;
    bool TryReadQuotaWindow(const JsonValue& rateLimits, const std::string& fieldName, QuotaWindow* window) const;
    int64_t ParseTimestampMs(const std::string& timestamp) const;
    int64_t CurrentTimeMs() const;

    std::map<std::wstring, FileCursor> cursors_;
    StatusSnapshot currentSnapshot_;
};
