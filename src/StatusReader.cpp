/**
 * 文件作用：实现 Codex JSONL 状态快照读取器
 * 职责范围：
 * 1. 只读扫描本机 rollout JSONL
 * 2. 将真实事件映射为会话状态
 * 3. 解析额度窗口并计算有效剩余额度
 *
 * 不负责：
 * - 根据文件静默时间推断等待用户
 * - 输出或保存用户提示词、模型回复、工具输出正文
 *
 * 维护说明：
 * - 所有托盘颜色和控制台状态都应来自这里生成的快照
 */
#include "StatusReader.h"

#include "CodexRootLocator.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>

StatusSnapshot StatusReader::ReadOnce(const StatusReadOptions& options)
{
    CodexRootLocator locator;
    const CodexRootResult rootResult = locator.Locate(options.codexHome);
    if (!rootResult.isUsable) {
        currentSnapshot_.hasSourceError = true;
        currentSnapshot_.errorMessage = rootResult.errorMessage;
        currentSnapshot_.codexRoot = rootResult.rootPath;
        currentSnapshot_.sessionsPath = rootResult.sessionsPath;
        return currentSnapshot_;
    }

    currentSnapshot_.hasSourceError = false;
    currentSnapshot_.errorMessage.clear();
    currentSnapshot_.codexRoot = rootResult.rootPath;
    currentSnapshot_.sessionsPath = rootResult.sessionsPath;

    const std::vector<RolloutFile> files = FindRecentRolloutFiles(
        rootResult.sessionsPath,
        options.maxFiles,
        options.recentHours);

    currentSnapshot_.trackedFileCount = files.size();
    for (const RolloutFile& file : files) {
        ProcessFile(file, &currentSnapshot_);
    }

    ApplyStalePolicy(options.staleMinutes, &currentSnapshot_);

    return currentSnapshot_;
}

std::vector<StatusReader::RolloutFile> StatusReader::FindRecentRolloutFiles(
    const std::wstring& sessionsPath,
    size_t maxFiles,
    int recentHours)
{
    std::vector<RolloutFile> files;
    std::error_code errorCode;
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(recentHours);

    std::filesystem::recursive_directory_iterator iterator(
        std::filesystem::path(sessionsPath),
        std::filesystem::directory_options::skip_permission_denied,
        errorCode);

    for (const auto end = std::filesystem::recursive_directory_iterator(); iterator != end && !errorCode; iterator.increment(errorCode)) {
        const std::filesystem::directory_entry& entry = *iterator;
        if (!entry.is_regular_file(errorCode) || errorCode) {
            errorCode.clear();
            continue;
        }

        const std::filesystem::path path = entry.path();
        const std::wstring fileName = path.filename().wstring();
        const bool isRolloutJsonl = fileName.rfind(L"rollout-", 0) == 0 && path.extension() == L".jsonl";
        if (!isRolloutJsonl) {
            continue;
        }

        const auto lastWriteTime = entry.last_write_time(errorCode);
        if (errorCode) {
            errorCode.clear();
            continue;
        }
        if (lastWriteTime < cutoff) {
            continue;
        }

        RolloutFile file;
        file.path = path;
        file.lastWriteTime = lastWriteTime;
        file.size = entry.file_size(errorCode);
        if (errorCode) {
            file.size = 0;
            errorCode.clear();
        }
        files.push_back(file);
    }

    std::sort(files.begin(), files.end(), [](const RolloutFile& left, const RolloutFile& right) {
        return left.lastWriteTime > right.lastWriteTime;
    });

    if (files.size() > maxFiles) {
        files.resize(maxFiles);
    }

    std::sort(files.begin(), files.end(), [](const RolloutFile& left, const RolloutFile& right) {
        return left.lastWriteTime < right.lastWriteTime;
    });

    return files;
}

void StatusReader::ProcessFile(const RolloutFile& file, StatusSnapshot* snapshot)
{
    FileCursor& cursor = cursors_[file.path.wstring()];
    if (cursor.path.empty()) {
        cursor.path = file.path;
    }

    if (file.size < cursor.offset) {
        RemoveSessionsFromFile(file.path, snapshot);
        cursor.offset = 0;
        cursor.knownSize = 0;
        cursor.incompleteLine.clear();
        cursor.consecutiveErrors = 0;
    }

    if (file.size == cursor.offset) {
        cursor.knownSize = file.size;
        return;
    }

    std::ifstream input(file.path, std::ios::binary);
    if (!input) {
        snapshot->hasSourceError = true;
        snapshot->errorMessage = L"rollout file is unreadable";
        return;
    }

    input.seekg(static_cast<std::streamoff>(cursor.offset), std::ios::beg);
    if (!input) {
        snapshot->hasSourceError = true;
        snapshot->errorMessage = L"failed to seek rollout file";
        cursor.offset = 0;
        cursor.incompleteLine.clear();
        return;
    }

    const uintmax_t startOffset = cursor.offset;
    std::string newBytes {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    cursor.offset = startOffset + static_cast<uintmax_t>(newBytes.size());
    cursor.knownSize = file.size;

    if (newBytes.empty()) {
        return;
    }

    std::string pendingText = cursor.incompleteLine;
    pendingText += newBytes;
    cursor.incompleteLine.clear();

    JsonParser parser;
    size_t lineStart = 0;
    while (lineStart < pendingText.size()) {
        const size_t newlineIndex = pendingText.find('\n', lineStart);
        if (newlineIndex == std::string::npos) {
            cursor.incompleteLine = pendingText.substr(lineStart);
            break;
        }

        std::string line = pendingText.substr(lineStart, newlineIndex - lineStart);
        lineStart = newlineIndex + 1;

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        JsonValue root;
        std::string error;
        if (!parser.Parse(line, &root, &error)) {
            snapshot->parseErrorCount++;
            cursor.consecutiveErrors++;
            if (cursor.consecutiveErrors >= 3) {
                snapshot->hasSourceError = true;
                snapshot->errorMessage = L"JSON parse errors reached the P3 safety threshold";
            }
            continue;
        }

        cursor.consecutiveErrors = 0;
        snapshot->parsedLineCount++;
        ProcessLine(root, file.path, snapshot);
    }
}

void StatusReader::ProcessLine(const JsonValue& root, const std::filesystem::path& sourceFile, StatusSnapshot* snapshot)
{
    NormalizedEvent event = NormalizeEvent(root, sourceFile);
    if (event.type == "QuotaUpdated") {
        UpdateQuota(root, event.timestamp, snapshot);
        return;
    }

    ApplyEvent(event, snapshot);
}

StatusReader::NormalizedEvent StatusReader::NormalizeEvent(const JsonValue& root, const std::filesystem::path& sourceFile)
{
    NormalizedEvent event;
    event.rawEventName = GetPayloadType(root);
    event.sessionId = GetSessionId(root, sourceFile);
    event.taskId = GetTaskId(root);
    event.callName = GetPayloadName(root);
    event.timestamp = GetTimestamp(root);
    event.timestampMs = ParseTimestampMs(event.timestamp);
    event.sourceFile = sourceFile.wstring();

    const std::string topType = GetTopType(root);
    if (topType == "session_meta") {
        event.rawEventName = "session_meta";
        event.type = "SessionDiscovered";
        return event;
    }
    if (event.rawEventName == "task_started") {
        event.type = "TaskStarted";
        return event;
    }
    if (event.rawEventName == "task_complete") {
        event.type = HasPayloadField(root, "error") ? "TaskFailed" : "TaskCompleted";
        return event;
    }
    if (event.rawEventName == "turn_aborted") {
        event.type = "TaskCancelled";
        return event;
    }
    if (event.rawEventName == "token_count") {
        event.type = "QuotaUpdated";
        return event;
    }
    if (event.rawEventName == "user_message") {
        event.type = "UserInputReceived";
        return event;
    }
    if (event.callName == "request_permissions") {
        event.type = "WaitingForApproval";
        return event;
    }
    if (event.callName == "request_user_input") {
        event.type = "WaitingForUserInput";
        return event;
    }
    if (event.rawEventName == "reasoning" ||
        event.rawEventName == "agent_reasoning" ||
        event.rawEventName == "agent_message") {
        event.type = "ModelActivity";
        return event;
    }
    if (event.rawEventName == "function_call" ||
        event.rawEventName == "custom_tool_call" ||
        event.rawEventName == "tool_search_call" ||
        event.rawEventName == "web_search_call") {
        event.type = "ToolStarted";
        return event;
    }
    if (event.rawEventName == "function_call_output" ||
        event.rawEventName == "custom_tool_call_output" ||
        event.rawEventName == "tool_search_output" ||
        event.rawEventName == "web_search_end" ||
        event.rawEventName == "mcp_tool_call_end" ||
        event.rawEventName == "patch_apply_end" ||
        event.rawEventName == "image_generation_end") {
        event.type = "ToolCompleted";
        return event;
    }

    event.type = "Unknown";
    return event;
}

void StatusReader::ApplyEvent(const NormalizedEvent& event, StatusSnapshot* snapshot)
{
    if (event.type == "Unknown") {
        snapshot->unknownEventCount++;
    } else {
        snapshot->recognizedEventCount++;
    }

    SessionState& session = snapshot->sessions[event.sessionId];
    if (session.sessionId.empty()) {
        session.sessionId = event.sessionId;
    }
    if (!event.sourceFile.empty()) {
        session.sourceFile = event.sourceFile;
    }
    if (!event.taskId.empty()) {
        session.taskId = event.taskId;
    }
    if (!event.rawEventName.empty()) {
        session.lastRawEvent = event.rawEventName;
    }
    if (!event.timestamp.empty()) {
        session.lastEventTime = event.timestamp;
    }
    if (event.timestampMs > 0) {
        session.lastEventMs = event.timestampMs;
    }

    if (event.type == "SessionDiscovered" || event.type == "Unknown") {
        return;
    }

    session.sawRecognizedEvent = true;

    if (event.type == "TaskStarted" ||
        event.type == "ModelActivity" ||
        event.type == "ToolStarted" ||
        event.type == "ToolCompleted" ||
        event.type == "UserInputReceived") {
        session.state = TaskState::Running;
        return;
    }
    if (event.type == "WaitingForApproval" || event.type == "WaitingForUserInput") {
        session.state = TaskState::WaitingInput;
        return;
    }
    if (event.type == "TaskCompleted") {
        session.state = TaskState::Completed;
        return;
    }
    if (event.type == "TaskFailed") {
        session.state = TaskState::Failed;
        return;
    }
    if (event.type == "TaskCancelled") {
        session.state = TaskState::Cancelled;
        return;
    }
}

void StatusReader::RemoveSessionsFromFile(const std::filesystem::path& sourceFile, StatusSnapshot* snapshot)
{
    const std::wstring sourcePath = sourceFile.wstring();
    for (auto iterator = snapshot->sessions.begin(); iterator != snapshot->sessions.end();) {
        if (iterator->second.sourceFile == sourcePath) {
            iterator = snapshot->sessions.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void StatusReader::UpdateQuota(const JsonValue& root, const std::string& timestamp, StatusSnapshot* snapshot)
{
    const JsonValue* payload = root.GetObjectField("payload");
    const JsonValue* rateLimits = payload == nullptr ? nullptr : payload->GetObjectField("rate_limits");
    if (rateLimits == nullptr || !rateLimits->IsObject()) {
        snapshot->quota.validity = QuotaValidity::Unavailable;
        snapshot->quota.receivedAt = timestamp;
        return;
    }

    QuotaSnapshot quota;
    quota.receivedAt = timestamp;
    quota.planType = GetNestedString(root, { "payload", "rate_limits", "plan_type" });

    QuotaWindow primary;
    QuotaWindow secondary;
    const bool hasPrimary = TryReadQuotaWindow(*rateLimits, "primary", &primary);
    const bool hasSecondary = TryReadQuotaWindow(*rateLimits, "secondary", &secondary);

    if (hasPrimary) {
        quota.primary = primary;
    }
    if (hasSecondary) {
        quota.secondary = secondary;
    }
    if (hasPrimary && hasSecondary) {
        quota.effectiveRemaining = std::min(primary.remainingPercent, secondary.remainingPercent);
        quota.validity = QuotaValidity::Valid;
    } else if (hasPrimary || hasSecondary) {
        quota.effectiveRemaining = hasPrimary ? primary.remainingPercent : secondary.remainingPercent;
        quota.validity = QuotaValidity::Partial;
    } else {
        quota.validity = QuotaValidity::Unavailable;
    }

    snapshot->quota = quota;
}

std::string StatusReader::GetTopType(const JsonValue& root) const
{
    const JsonValue* type = root.GetObjectField("type");
    return type == nullptr ? std::string() : type->GetStringOrEmpty();
}

std::string StatusReader::GetPayloadType(const JsonValue& root) const
{
    return GetNestedString(root, { "payload", "type" });
}

std::string StatusReader::GetPayloadName(const JsonValue& root) const
{
    return GetNestedString(root, { "payload", "name" });
}

std::string StatusReader::GetTimestamp(const JsonValue& root) const
{
    const JsonValue* timestamp = root.GetObjectField("timestamp");
    return timestamp == nullptr ? std::string() : timestamp->GetStringOrEmpty();
}

std::string StatusReader::GetSessionId(const JsonValue& root, const std::filesystem::path& sourceFile) const
{
    const std::string sessionMetaId = GetNestedString(root, { "payload", "id" });
    if (GetTopType(root) == "session_meta" && !sessionMetaId.empty()) {
        return sessionMetaId;
    }

    const std::string threadId = GetNestedString(root, { "payload", "thread_id" });
    if (!threadId.empty()) {
        return threadId;
    }

    const std::string stem = sourceFile.stem().string();
    if (stem.size() >= 36) {
        return stem.substr(stem.size() - 36);
    }
    return stem;
}

std::string StatusReader::GetTaskId(const JsonValue& root) const
{
    const std::string turnId = GetNestedString(root, { "payload", "turn_id" });
    if (!turnId.empty()) {
        return turnId;
    }
    return GetNestedString(root, { "payload", "internal_chat_message_metadata_passthrough", "turn_id" });
}

std::string StatusReader::GetNestedString(const JsonValue& root, const std::vector<std::string>& path) const
{
    const JsonValue* current = &root;
    for (const std::string& fieldName : path) {
        current = current->GetObjectField(fieldName);
        if (current == nullptr) {
            return std::string();
        }
    }
    return current->GetStringOrEmpty();
}

bool StatusReader::HasPayloadField(const JsonValue& root, const std::string& fieldName) const
{
    const JsonValue* payload = root.GetObjectField("payload");
    return payload != nullptr && payload->IsObject() && payload->GetObjectField(fieldName) != nullptr;
}

bool StatusReader::TryReadQuotaWindow(const JsonValue& rateLimits, const std::string& fieldName, QuotaWindow* window) const
{
    const JsonValue* value = rateLimits.GetObjectField(fieldName);
    if (value == nullptr || !value->IsObject()) {
        return false;
    }

    const JsonValue* usedPercent = value->GetObjectField("used_percent");
    const JsonValue* windowMinutes = value->GetObjectField("window_minutes");
    const JsonValue* resetAt = value->GetObjectField("resets_at");
    if (usedPercent == nullptr || !usedPercent->IsNumber()) {
        return false;
    }

    window->usedPercent = usedPercent->numberValue;
    window->remainingPercent = 100.0 - window->usedPercent;
    if (window->remainingPercent < 0.0) {
        window->remainingPercent = 0.0;
    }
    if (window->remainingPercent > 100.0) {
        window->remainingPercent = 100.0;
    }
    if (windowMinutes != nullptr && windowMinutes->IsNumber()) {
        window->windowMinutes = static_cast<int>(windowMinutes->numberValue);
    }
    if (resetAt != nullptr && resetAt->IsNumber()) {
        window->resetAt = static_cast<int64_t>(resetAt->numberValue);
    }

    return true;
}

void StatusReader::ApplyStalePolicy(int staleMinutes, StatusSnapshot* snapshot) const
{
    const int64_t nowMs = CurrentTimeMs();
    const int64_t staleMs = static_cast<int64_t>(staleMinutes) * 60 * 1000;

    for (auto& item : snapshot->sessions) {
        SessionState& session = item.second;
        if (session.state != TaskState::Running) {
            continue;
        }
        if (session.lastEventMs <= 0) {
            continue;
        }

        const bool isOlderThanStaleLimit = nowMs - session.lastEventMs > staleMs;
        if (isOlderThanStaleLimit) {
            session.state = TaskState::Stale;
        }
    }
}

int64_t StatusReader::ParseTimestampMs(const std::string& timestamp) const
{
    if (timestamp.empty()) {
        return 0;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    const int parsedCount = sscanf_s(
        timestamp.c_str(),
        "%d-%d-%dT%d:%d:%d",
        &year,
        &month,
        &day,
        &hour,
        &minute,
        &second);

    if (parsedCount != 6) {
        return 0;
    }

    std::tm utcTime {};
    utcTime.tm_year = year - 1900;
    utcTime.tm_mon = month - 1;
    utcTime.tm_mday = day;
    utcTime.tm_hour = hour;
    utcTime.tm_min = minute;
    utcTime.tm_sec = second;

    const time_t epochSeconds = _mkgmtime(&utcTime);
    if (epochSeconds <= 0) {
        return 0;
    }

    return static_cast<int64_t>(epochSeconds) * 1000;
}

int64_t StatusReader::CurrentTimeMs() const
{
    const auto now = std::chrono::system_clock::now();
    const auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}
