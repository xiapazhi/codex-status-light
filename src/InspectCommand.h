/**
 * 文件作用：声明 StatusLight.exe --inspect 取证命令
 * 职责范围：
 * 1. 扫描最近 rollout JSONL 文件
 * 2. 输出脱敏后的事件结构摘要
 * 3. 保存未知事件的脱敏样本
 *
 * 不负责：
 * - 托盘 UI
 * - Codex 进程控制
 *
 * 维护说明：
 * - 后续正式状态机只能依赖已经在 docs/codex-event-map.md 记录的稳定映射
 */
#pragma once

#include "JsonValue.h"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

struct InspectOptions {
    std::wstring codexHome;
    size_t maxFiles = 30;
    int recentHours = 24;
    int pollSeconds = 2;
    bool watch = false;
    bool saveUnknownSamples = true;
};

class InspectCommand {
public:
    int Run(const InspectOptions& options);

private:
    struct RolloutFile {
        std::filesystem::path path;
        std::filesystem::file_time_type lastWriteTime;
        uintmax_t size = 0;
    };

    struct ParsedEvent {
        std::string topType;
        std::string payloadType;
        std::string normalizedType;
        std::string sessionId;
        std::string turnId;
        std::string callName;
        std::string callId;
        std::string status;
        std::string timestamp;
        std::vector<std::string> fieldPaths;
    };

    std::vector<RolloutFile> FindRecentRolloutFiles(const std::wstring& sessionsPath, size_t maxFiles, int recentHours);
    uint64_t InspectFile(const RolloutFile& file, uint64_t startOffset, bool printFileHeader);
    ParsedEvent ParseEvent(const JsonValue& root, const std::filesystem::path& sourceFile);
    std::string NormalizeEvent(const ParsedEvent& event);
    void CollectFieldPaths(const JsonValue& value, const std::string& prefix, std::vector<std::string>* paths);
    void SaveUnknownSample(const JsonValue& root, const std::filesystem::path& sourceFile, uint64_t offset);
    std::string BuildSanitizedJson(const JsonValue& value, const std::string& keyName, int depth);
    std::string JsonEscape(const std::string& value);
    std::string NarrowPath(const std::filesystem::path& path);
    bool IsSensitiveField(const std::string& keyName);
    bool ShouldStopAtField(const std::string& keyName);
    std::string GetStringField(const JsonValue& object, const std::string& name);
    std::string GetSessionIdFromPath(const std::filesystem::path& sourceFile);
    bool FileEndsWithNewline(const std::filesystem::path& path);

    std::filesystem::path unknownSampleDirectory_;
    std::set<std::string> savedUnknownKeys_;
    std::map<std::string, size_t> normalizedCounts_;
    size_t parsedLineCount_ = 0;
    size_t parseErrorCount_ = 0;
};
