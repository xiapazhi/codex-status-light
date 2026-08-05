/**
 * 文件作用：实现 Codex 数据根目录定位和只读校验
 * 职责范围：
 * 1. 按参数、环境变量和默认用户目录定位 .codex
 * 2. 检查目录存在性和可读性
 * 3. 生成清晰错误信息供取证模式输出
 *
 * 不负责：
 * - 创建 sessions 目录
 * - 修改 Codex 配置或状态文件
 *
 * 维护说明：
 * - 命令行参数用于本地取证显式覆盖，避免调试时误读默认账户目录
 */
#include "CodexRootLocator.h"

#include <Windows.h>

#include <filesystem>
#include <vector>

namespace {

std::wstring JoinPath(const std::wstring& left, const std::wstring& right)
{
    return (std::filesystem::path(left) / right).wstring();
}

} // namespace

CodexRootResult CodexRootLocator::Locate(const std::wstring& commandLineCodexHome) const
{
    std::vector<std::wstring> candidates;

    // 显式参数只用于本次取证运行，优先级最高可以避免误扫默认 .codex。
    if (!commandLineCodexHome.empty()) {
        candidates.push_back(commandLineCodexHome);
    }

    const std::wstring envCodexHome = ReadEnvironmentVariable(L"CODEX_HOME");
    if (!envCodexHome.empty()) {
        candidates.push_back(envCodexHome);
    }

    const std::wstring userProfile = ReadEnvironmentVariable(L"USERPROFILE");
    if (!userProfile.empty()) {
        candidates.push_back(JoinPath(userProfile, L".codex"));
    }

    for (const std::wstring& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }

        if (!IsReadableDirectory(candidate)) {
            continue;
        }

        const std::wstring sessionsPath = JoinPath(candidate, L"sessions");
        if (!IsReadableDirectory(sessionsPath)) {
            CodexRootResult result;
            result.rootPath = candidate;
            result.sessionsPath = sessionsPath;
            result.errorMessage = L"Codex sessions directory is missing or unreadable";
            return result;
        }

        CodexRootResult result;
        result.rootPath = candidate;
        result.sessionsPath = sessionsPath;
        result.isUsable = true;
        return result;
    }

    CodexRootResult result;
    result.errorMessage = L"Codex root directory was not found or is unreadable";
    return result;
}

std::wstring CodexRootLocator::ReadEnvironmentVariable(const wchar_t* name) const
{
    const DWORD requiredSize = GetEnvironmentVariableW(name, nullptr, 0);
    if (requiredSize == 0) {
        return std::wstring();
    }

    std::wstring value(requiredSize, L'\0');
    const DWORD writtenSize = GetEnvironmentVariableW(name, value.data(), requiredSize);
    if (writtenSize == 0) {
        return std::wstring();
    }

    value.resize(writtenSize);
    return value;
}

bool CodexRootLocator::IsReadableDirectory(const std::wstring& path) const
{
    std::error_code errorCode;
    const std::filesystem::path directory(path);
    if (!std::filesystem::exists(directory, errorCode) || errorCode) {
        return false;
    }

    if (!std::filesystem::is_directory(directory, errorCode) || errorCode) {
        return false;
    }

    std::filesystem::directory_iterator iterator(directory, errorCode);
    return !errorCode;
}
