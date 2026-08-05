/**
 * 文件作用：实现 P0 JSONL 取证命令
 * 职责范围：
 * 1. 只读扫描本机 Codex rollout JSONL
 * 2. 增量式按行解析并记录源文件字节偏移
 * 3. 输出事件类型、时间、会话标识、字段路径和规范化类型
 * 4. 保存未知事件的脱敏样本，便于补充事件映射表
 *
 * 不负责：
 * - 根据文件静默时间推断任务状态
 * - 读取或输出用户提示词、模型回复正文、工具输出正文
 *
 * 维护说明：
 * - 新增事件映射前必须先用本命令采集真实样本并更新 docs/codex-event-map.md
 */
#include "InspectCommand.h"

#include "CodexRootLocator.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace {

std::string JoinStrings(const std::vector<std::string>& values, const std::string& separator)
{
    std::ostringstream output;
    for (size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            output << separator;
        }
        output << values[index];
    }
    return output.str();
}

std::string ToLowerAscii(const std::string& value)
{
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char current) {
        return static_cast<char>(std::tolower(current));
    });
    return lowered;
}

} // namespace

int InspectCommand::Run(const InspectOptions& options)
{
    CodexRootLocator locator;
    const CodexRootResult rootResult = locator.Locate(options.codexHome);
    if (!rootResult.isUsable) {
        std::wcerr << L"inspect failed: " << rootResult.errorMessage << L"\n";
        return 2;
    }

    unknownSampleDirectory_ = std::filesystem::current_path() / "diagnostics" / "unknown-events";

    const std::vector<RolloutFile> files = FindRecentRolloutFiles(
        rootResult.sessionsPath,
        options.maxFiles,
        options.recentHours);

    std::wcout << L"Codex root: " << rootResult.rootPath << L"\n";
    std::wcout << L"Sessions: " << rootResult.sessionsPath << L"\n";
    std::cout << "Rollout files selected: " << files.size() << "\n\n";

    std::map<std::filesystem::path, uint64_t> cursors;
    for (const RolloutFile& file : files) {
        cursors[file.path] = InspectFile(file, 0, true);
    }

    if (options.watch) {
        std::cout << "\nWatching for new complete JSONL lines. Press Ctrl+C to stop.\n";
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(options.pollSeconds));

            const std::vector<RolloutFile> updatedFiles = FindRecentRolloutFiles(
                rootResult.sessionsPath,
                options.maxFiles,
                options.recentHours);

            for (const RolloutFile& file : updatedFiles) {
                uint64_t startOffset = 0;
                const auto cursor = cursors.find(file.path);
                if (cursor != cursors.end()) {
                    startOffset = cursor->second;
                }

                // 文件可能被截断或替换，游标超过当前大小时从头重新取证。
                if (file.size < startOffset) {
                    startOffset = 0;
                }

                cursors[file.path] = InspectFile(file, startOffset, cursor == cursors.end());
            }
        }
    }

    std::cout << "\nSummary\n";
    std::cout << "Parsed lines: " << parsedLineCount_ << "\n";
    std::cout << "Parse errors: " << parseErrorCount_ << "\n";
    for (const auto& item : normalizedCounts_) {
        std::cout << item.first << ": " << item.second << "\n";
    }

    return parseErrorCount_ > 0 ? 1 : 0;
}

std::vector<InspectCommand::RolloutFile> InspectCommand::FindRecentRolloutFiles(
    const std::wstring& sessionsPath,
    size_t maxFiles,
    int recentHours)
{
    std::vector<RolloutFile> files;
    std::error_code errorCode;
    const std::filesystem::path root(sessionsPath);
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(recentHours);

    std::filesystem::recursive_directory_iterator iterator(
        root,
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

    return files;
}

uint64_t InspectCommand::InspectFile(const RolloutFile& file, uint64_t startOffset, bool printFileHeader)
{
    std::ifstream input(file.path, std::ios::binary);
    if (!input) {
        std::cout << "file unreadable: " << NarrowPath(file.path) << "\n";
        return startOffset;
    }

    if (startOffset > 0) {
        input.seekg(static_cast<std::streamoff>(startOffset), std::ios::beg);
    }

    if (printFileHeader) {
        std::cout << "File: " << NarrowPath(file.path.filename()) << "\n";
    }

    JsonParser parser;
    std::string line;
    uint64_t lineNumber = 0;
    uint64_t nextOffset = startOffset;
    const bool endsWithNewline = FileEndsWithNewline(file.path);
    while (true) {
        const std::streampos currentOffset = input.tellg();
        if (!std::getline(input, line)) {
            break;
        }

        lineNumber++;
        const bool isLastIncompleteLine = input.eof() && !endsWithNewline;
        if (isLastIncompleteLine) {
            return static_cast<uint64_t>(currentOffset);
        }

        const std::streampos afterLineOffset = input.tellg();
        if (afterLineOffset == std::streampos(-1)) {
            nextOffset = file.size;
        } else {
            nextOffset = static_cast<uint64_t>(afterLineOffset);
        }

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        JsonValue root;
        std::string error;
        if (!parser.Parse(line, &root, &error)) {
            parseErrorCount_++;
            std::cout << "  line=" << lineNumber
                << " offset=" << static_cast<uint64_t>(currentOffset)
                << " parse_error=" << error << "\n";
            continue;
        }

        parsedLineCount_++;
        ParsedEvent event = ParseEvent(root, file.path);
        event.normalizedType = NormalizeEvent(event);
        normalizedCounts_[event.normalizedType]++;

        std::cout << "  line=" << lineNumber
            << " offset=" << static_cast<uint64_t>(currentOffset)
            << " timestamp=" << event.timestamp
            << " type=" << event.topType;

        if (!event.payloadType.empty()) {
            std::cout << " payload.type=" << event.payloadType;
        }
        if (!event.callName.empty()) {
            std::cout << " name=" << event.callName;
        }
        if (!event.status.empty()) {
            std::cout << " status=" << event.status;
        }
        if (!event.sessionId.empty()) {
            std::cout << " session=" << event.sessionId;
        }
        if (!event.turnId.empty()) {
            std::cout << " turn=" << event.turnId;
        }

        std::cout << " normalized=" << event.normalizedType
            << " fields=" << JoinStrings(event.fieldPaths, ",")
            << "\n";

        if (event.normalizedType == "Unknown") {
            SaveUnknownSample(root, file.path, static_cast<uint64_t>(currentOffset));
        }
    }

    return nextOffset;
}

InspectCommand::ParsedEvent InspectCommand::ParseEvent(const JsonValue& root, const std::filesystem::path& sourceFile)
{
    ParsedEvent event;
    event.sessionId = GetSessionIdFromPath(sourceFile);

    event.topType = GetStringField(root, "type");
    event.timestamp = GetStringField(root, "timestamp");

    const JsonValue* payload = root.GetObjectField("payload");
    if (payload != nullptr && payload->IsObject()) {
        event.payloadType = GetStringField(*payload, "type");
        event.turnId = GetStringField(*payload, "turn_id");
        event.callName = GetStringField(*payload, "name");
        event.callId = GetStringField(*payload, "call_id");
        event.status = GetStringField(*payload, "status");

        const std::string threadId = GetStringField(*payload, "thread_id");
        if (!threadId.empty()) {
            event.sessionId = threadId;
        }

        const std::string sessionMetaId = GetStringField(*payload, "id");
        if (event.topType == "session_meta" && !sessionMetaId.empty()) {
            event.sessionId = sessionMetaId;
        }
    }

    CollectFieldPaths(root, "", &event.fieldPaths);
    return event;
}

std::string InspectCommand::NormalizeEvent(const ParsedEvent& event)
{
    if (event.topType == "session_meta") {
        return "SessionDiscovered";
    }

    if (event.payloadType == "task_started") {
        return "TaskStarted";
    }

    if (event.payloadType == "task_complete") {
        const bool hasErrorField = std::find(event.fieldPaths.begin(), event.fieldPaths.end(), "payload.error") != event.fieldPaths.end();
        return hasErrorField ? "TaskFailed" : "TaskCompleted";
    }

    if (event.payloadType == "turn_aborted") {
        return "TaskCancelled";
    }

    if (event.payloadType == "token_count") {
        return "QuotaUpdated";
    }

    if (event.payloadType == "user_message") {
        return "UserInputReceived";
    }

    if (event.callName == "request_permissions") {
        return "WaitingForApproval";
    }

    if (event.callName == "request_user_input") {
        return "WaitingForUserInput";
    }

    if (event.payloadType == "reasoning" || event.payloadType == "agent_reasoning" || event.payloadType == "agent_message") {
        return "ModelActivity";
    }

    if (event.payloadType == "function_call" ||
        event.payloadType == "custom_tool_call" ||
        event.payloadType == "tool_search_call" ||
        event.payloadType == "web_search_call") {
        return "ToolStarted";
    }

    if (event.payloadType == "function_call_output" ||
        event.payloadType == "custom_tool_call_output" ||
        event.payloadType == "tool_search_output" ||
        event.payloadType == "web_search_end" ||
        event.payloadType == "mcp_tool_call_end" ||
        event.payloadType == "patch_apply_end" ||
        event.payloadType == "image_generation_end") {
        return "ToolCompleted";
    }

    return "Unknown";
}

void InspectCommand::CollectFieldPaths(const JsonValue& value, const std::string& prefix, std::vector<std::string>* paths)
{
    if (!value.IsObject()) {
        return;
    }

    for (const auto& item : value.objectValue) {
        const std::string path = prefix.empty() ? item.first : prefix + "." + item.first;
        paths->push_back(path);
        if (paths->size() >= 120) {
            paths->push_back("<truncated>");
            return;
        }

        // 正文和工具参数可能包含用户输入、模型回复或命令输出；取证时只记录字段存在，不继续展开。
        if (ShouldStopAtField(item.first)) {
            continue;
        }

        if (item.second.IsObject()) {
            CollectFieldPaths(item.second, path, paths);
        } else if (item.second.IsArray()) {
            for (size_t index = 0; index < item.second.arrayValue.size() && index < 3; ++index) {
                if (item.second.arrayValue[index].IsObject()) {
                    CollectFieldPaths(item.second.arrayValue[index], path + "[]", paths);
                }
            }
        }
    }
}

void InspectCommand::SaveUnknownSample(const JsonValue& root, const std::filesystem::path& sourceFile, uint64_t offset)
{
    const std::string sampleKey = NarrowPath(sourceFile.filename()) + ":" + std::to_string(offset);
    if (savedUnknownKeys_.find(sampleKey) != savedUnknownKeys_.end()) {
        return;
    }

    if (savedUnknownKeys_.size() >= 50) {
        return;
    }

    std::error_code errorCode;
    std::filesystem::create_directories(unknownSampleDirectory_, errorCode);
    if (errorCode) {
        return;
    }

    const std::filesystem::path outputPath = unknownSampleDirectory_ /
        (sourceFile.stem().string() + "-" + std::to_string(offset) + ".json");

    std::ofstream output(outputPath, std::ios::binary);
    if (!output) {
        return;
    }

    output << BuildSanitizedJson(root, "", 0) << "\n";
    savedUnknownKeys_.insert(sampleKey);
}

std::string InspectCommand::BuildSanitizedJson(const JsonValue& value, const std::string& keyName, int depth)
{
    if (depth > 12) {
        return "\"<max-depth>\"";
    }

    if (ShouldStopAtField(keyName)) {
        return "\"<redacted>\"";
    }

    if (value.IsNull()) {
        return "null";
    }
    if (value.IsBool()) {
        return value.boolValue ? "true" : "false";
    }
    if (value.IsNumber()) {
        std::ostringstream output;
        output << value.numberValue;
        return output.str();
    }
    if (value.IsString()) {
        return "\"" + JsonEscape(value.stringValue) + "\"";
    }
    if (value.IsArray()) {
        std::vector<std::string> items;
        const size_t maxItems = std::min<size_t>(value.arrayValue.size(), 5);
        for (size_t index = 0; index < maxItems; ++index) {
            items.push_back(BuildSanitizedJson(value.arrayValue[index], keyName, depth + 1));
        }
        if (value.arrayValue.size() > maxItems) {
            items.push_back("\"<truncated>\"");
        }
        return "[" + JoinStrings(items, ",") + "]";
    }

    std::vector<std::string> fields;
    for (const auto& item : value.objectValue) {
        fields.push_back("\"" + JsonEscape(item.first) + "\":" + BuildSanitizedJson(item.second, item.first, depth + 1));
    }
    return "{" + JoinStrings(fields, ",") + "}";
}

std::string InspectCommand::JsonEscape(const std::string& value)
{
    std::ostringstream output;
    for (const char current : value) {
        switch (current) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(current) < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(current));
            } else {
                output << current;
            }
            break;
        }
    }
    return output.str();
}

std::string InspectCommand::NarrowPath(const std::filesystem::path& path)
{
    const std::wstring widePath = path.wstring();
    if (widePath.empty()) {
        return std::string();
    }

    const int requiredSize = WideCharToMultiByte(CP_UTF8, 0, widePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (requiredSize <= 0) {
        return path.string();
    }

    std::string result(static_cast<size_t>(requiredSize), '\0');
    WideCharToMultiByte(CP_UTF8, 0, widePath.c_str(), -1, result.data(), requiredSize, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

bool InspectCommand::IsSensitiveField(const std::string& keyName)
{
    const std::string lowered = ToLowerAscii(keyName);
    return lowered == "prompt" ||
        lowered == "message" ||
        lowered == "content" ||
        lowered == "text" ||
        lowered == "output" ||
        lowered == "stdout" ||
        lowered == "stderr" ||
        lowered == "arguments" ||
        lowered == "encrypted_content" ||
        lowered == "base_instructions";
}

bool InspectCommand::ShouldStopAtField(const std::string& keyName)
{
    if (IsSensitiveField(keyName)) {
        return true;
    }

    const std::string lowered = ToLowerAscii(keyName);
    return lowered == "dynamic_tools" ||
        lowered == "tools" ||
        lowered == "inputschema" ||
        lowered == "thread_settings" ||
        lowered == "permission_profile" ||
        lowered == "collaboration_mode";
}

std::string InspectCommand::GetStringField(const JsonValue& object, const std::string& name)
{
    const JsonValue* field = object.GetObjectField(name);
    if (field == nullptr || !field->IsString()) {
        return std::string();
    }
    return field->stringValue;
}

std::string InspectCommand::GetSessionIdFromPath(const std::filesystem::path& sourceFile)
{
    const std::string stem = sourceFile.stem().string();
    if (stem.size() >= 36) {
        return stem.substr(stem.size() - 36);
    }

    return stem;
}

bool InspectCommand::FileEndsWithNewline(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return true;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size <= 0) {
        return true;
    }

    input.seekg(size - 1, std::ios::beg);
    char last = '\0';
    input.get(last);
    return last == '\n';
}
