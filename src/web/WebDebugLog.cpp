/**
 * 文件作用：实现 Web 监听链路的本地调试日志工具
 * 职责范围：
 * 1. 将日志追加写入用户本地目录
 * 2. 自动创建 StatusLight 日志目录
 * 3. 统一输出时间、进程 ID 和组件名
 *
 * 不负责：
 * - 日志轮转和压缩
 * - 采集网页正文、Prompt、回复正文或 Cookie
 *
 * 维护说明：
 * - 此日志用于排查扩展和桌面端连接问题，写入失败不能影响主流程
 */
#include "WebDebugLog.h"

#include "NativeHostProtocol.h"

#include <Windows.h>

#include <sstream>

namespace {

std::wstring JoinPath(const std::wstring& left, const std::wstring& right)
{
    if (left.empty()) {
        return right;
    }
    if (left.back() == L'\\' || left.back() == L'/') {
        return left + right;
    }
    return left + L"\\" + right;
}

std::wstring LocalAppData()
{
    wchar_t buffer[MAX_PATH] {};
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(_countof(buffer)));
    if (size == 0 || size >= _countof(buffer)) {
        return std::wstring();
    }
    return buffer;
}

bool EnsureDirectory(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }

    std::wstring current;
    for (wchar_t item : path) {
        current.push_back(item);
        if (item != L'\\' && item != L'/') {
            continue;
        }
        if (current.size() > 3) {
            CreateDirectoryW(current.c_str(), nullptr);
        }
    }

    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring LogDirectory()
{
    const std::wstring localAppData = LocalAppData();
    if (!localAppData.empty()) {
        return JoinPath(localAppData, L"StatusLight\\logs");
    }

    wchar_t exePath[MAX_PATH] {};
    const DWORD size = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(_countof(exePath)));
    if (size == 0 || size >= _countof(exePath)) {
        return L".";
    }

    std::wstring path = exePath;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

std::wstring CurrentTimeText()
{
    SYSTEMTIME time {};
    GetLocalTime(&time);

    std::wostringstream output;
    output
        << time.wYear << L"-"
        << (time.wMonth < 10 ? L"0" : L"") << time.wMonth << L"-"
        << (time.wDay < 10 ? L"0" : L"") << time.wDay << L" "
        << (time.wHour < 10 ? L"0" : L"") << time.wHour << L":"
        << (time.wMinute < 10 ? L"0" : L"") << time.wMinute << L":"
        << (time.wSecond < 10 ? L"0" : L"") << time.wSecond << L"."
        << time.wMilliseconds;
    return output.str();
}

} // namespace

std::wstring WebDebugLog::LogPath()
{
    return JoinPath(LogDirectory(), L"web-bridge.log");
}

void WebDebugLog::Write(const wchar_t* component, const std::wstring& message)
{
    const std::wstring logDirectory = LogDirectory();
    if (!EnsureDirectory(logDirectory)) {
        return;
    }

    std::wostringstream line;
    line
        << CurrentTimeText()
        << L" pid=" << GetCurrentProcessId()
        << L" [" << component << L"] "
        << message
        << L"\r\n";

    const std::string body = NativeHostProtocol::WideToUtf8(line.str());
    HANDLE file = CreateFileW(
        LogPath().c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD bytesWritten = 0;
    WriteFile(file, body.data(), static_cast<DWORD>(body.size()), &bytesWritten, nullptr);
    CloseHandle(file);
}

void WebDebugLog::WriteUtf8(const wchar_t* component, const std::string& message)
{
    Write(component, NativeHostProtocol::Utf8ToWide(message));
}
