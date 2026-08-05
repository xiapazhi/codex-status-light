/**
 * 文件作用：实现 Chrome Native Messaging Host 注册器
 * 职责范围：
 * 1. 将 com.statuslight.web manifest 写入用户本地目录
 * 2. 将 manifest 路径注册到 HKCU
 * 3. 固定 allowed_origins 到当前开发版扩展 ID
 *
 * 不负责：
 * - 保存扩展私钥
 * - 写 HKLM 或请求管理员权限
 *
 * 维护说明：
 * - manifest 中只能包含 EXE 路径和固定扩展来源，不能写入会话状态
 */
#include "NativeHostRegistration.h"

#include "NativeHostProtocol.h"
#include "WebDebugLog.h"

#include <Windows.h>

#include <sstream>

namespace {

std::wstring LastWindowsError()
{
    std::wostringstream output;
    output << L"Windows error " << GetLastError();
    return output.str();
}

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

} // namespace

bool NativeHostRegistration::EnsureRegistered(std::wstring* errorMessage) const
{
    const std::wstring manifestPath = ManifestPath();
    const std::wstring exePath = CurrentExecutablePath();
    if (manifestPath.empty() || exePath.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"failed to resolve native host manifest or executable path";
        }
        WebDebugLog::Write(L"Registration", L"failed resolve manifest_or_exe_path");
        return false;
    }

    const std::wstring manifestDir = manifestPath.substr(0, manifestPath.find_last_of(L"\\/"));
    if (!EnsureDirectory(manifestDir, errorMessage)) {
        WebDebugLog::Write(L"Registration", L"failed create manifest_dir=" + manifestDir);
        return false;
    }
    if (!WriteManifest(manifestPath, exePath, errorMessage)) {
        WebDebugLog::Write(L"Registration", L"failed write manifest=" + manifestPath);
        return false;
    }
    if (!WriteRegistry(manifestPath, errorMessage)) {
        WebDebugLog::Write(L"Registration", L"failed write hkcu native_messaging_host");
        return false;
    }

    WebDebugLog::Write(L"Registration", L"ok manifest=" + manifestPath + L" exe=" + exePath);
    return true;
}

std::wstring NativeHostRegistration::ManifestPath() const
{
    const std::wstring localAppData = LocalAppData();
    if (localAppData.empty()) {
        return std::wstring();
    }

    return JoinPath(
        JoinPath(localAppData, L"StatusLight\\NativeMessaging"),
        L"com.statuslight.web.json");
}

std::wstring NativeHostRegistration::CurrentExecutablePath() const
{
    wchar_t buffer[MAX_PATH] {};
    const DWORD size = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(_countof(buffer)));
    if (size == 0 || size >= _countof(buffer)) {
        return std::wstring();
    }
    return buffer;
}

bool NativeHostRegistration::EnsureDirectory(const std::wstring& path, std::wstring* errorMessage) const
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

    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        if (errorMessage != nullptr) {
            *errorMessage = LastWindowsError();
        }
        return false;
    }

    return true;
}

bool NativeHostRegistration::WriteManifest(
    const std::wstring& manifestPath,
    const std::wstring& exePath,
    std::wstring* errorMessage) const
{
    const std::string escapedExePath = NativeHostProtocol::JsonEscape(NativeHostProtocol::WideToUtf8(exePath));
    const std::string allowedOrigin = NativeHostProtocol::JsonEscape(
        NativeHostProtocol::WideToUtf8(NativeHostProtocol::kAllowedExtensionOrigin));

    std::ostringstream json;
    json << "{\n"
        << "  \"name\": \"com.statuslight.web\",\n"
        << "  \"description\": \"StatusLight ChatGPT Web bridge\",\n"
        << "  \"path\": \"" << escapedExePath << "\",\n"
        << "  \"type\": \"stdio\",\n"
        << "  \"allowed_origins\": [\n"
        << "    \"" << allowedOrigin << "\"\n"
        << "  ]\n"
        << "}\n";

    HANDLE file = CreateFileW(
        manifestPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (errorMessage != nullptr) {
            *errorMessage = LastWindowsError();
        }
        return false;
    }

    const std::string body = json.str();
    DWORD bytesWritten = 0;
    const BOOL written = WriteFile(
        file,
        body.data(),
        static_cast<DWORD>(body.size()),
        &bytesWritten,
        nullptr);
    CloseHandle(file);

    if (!written || bytesWritten != body.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"failed to write complete native host manifest";
        }
        return false;
    }

    return true;
}

bool NativeHostRegistration::WriteRegistry(const std::wstring& manifestPath, std::wstring* errorMessage) const
{
    const std::wstring keyPath =
        L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.statuslight.web";

    HKEY key = nullptr;
    const LONG createResult = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        keyPath.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (createResult != ERROR_SUCCESS) {
        if (errorMessage != nullptr) {
            std::wostringstream output;
            output << L"failed to create native host registry key: " << createResult;
            *errorMessage = output.str();
        }
        return false;
    }

    const DWORD byteCount = static_cast<DWORD>((manifestPath.size() + 1) * sizeof(wchar_t));
    const LONG setResult = RegSetValueExW(
        key,
        nullptr,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(manifestPath.c_str()),
        byteCount);
    RegCloseKey(key);

    if (setResult != ERROR_SUCCESS) {
        if (errorMessage != nullptr) {
            std::wostringstream output;
            output << L"failed to write native host registry value: " << setResult;
            *errorMessage = output.str();
        }
        return false;
    }

    return true;
}
