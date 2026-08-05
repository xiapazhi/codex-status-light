/**
 * 文件作用：实现 Chrome Native Messaging 与 Named Pipe 共用协议工具
 * 职责范围：
 * 1. 使用 Windows 小端 uint32 长度帧读写 UTF-8 JSON
 * 2. 生成按当前用户 SID 隔离的 Named Pipe 名称
 * 3. 提供最小 JSON 响应构造能力
 *
 * 不负责：
 * - 使用正则解析 JSON
 * - 输出任何聊天正文或浏览器敏感信息
 *
 * 维护说明：
 * - 消息体超过 64 KB 直接拒绝，网页状态消息不应接近该上限
 */
#include "NativeHostProtocol.h"

#include <Sddl.h>

#include <sstream>
#include <vector>

namespace {

std::wstring LastWindowsError()
{
    std::wostringstream output;
    output << L"Windows error " << GetLastError();
    return output.str();
}

bool ReadExact(HANDLE input, void* buffer, DWORD byteCount)
{
    char* cursor = static_cast<char*>(buffer);
    DWORD totalRead = 0;
    while (totalRead < byteCount) {
        DWORD bytesRead = 0;
        if (!ReadFile(input, cursor + totalRead, byteCount - totalRead, &bytesRead, nullptr)) {
            return false;
        }
        if (bytesRead == 0) {
            return false;
        }
        totalRead += bytesRead;
    }
    return true;
}

bool WriteExact(HANDLE output, const void* buffer, DWORD byteCount)
{
    const char* cursor = static_cast<const char*>(buffer);
    DWORD totalWritten = 0;
    while (totalWritten < byteCount) {
        DWORD bytesWritten = 0;
        if (!WriteFile(output, cursor + totalWritten, byteCount - totalWritten, &bytesWritten, nullptr)) {
            return false;
        }
        if (bytesWritten == 0) {
            return false;
        }
        totalWritten += bytesWritten;
    }
    return true;
}

std::wstring CurrentUserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return L"unknown";
    }

    DWORD requiredSize = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &requiredSize);
    std::vector<BYTE> buffer(requiredSize);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), requiredSize, &requiredSize)) {
        CloseHandle(token);
        return L"unknown";
    }

    TOKEN_USER* tokenUser = reinterpret_cast<TOKEN_USER*>(buffer.data());
    wchar_t* sidText = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sidText)) {
        CloseHandle(token);
        return L"unknown";
    }

    std::wstring result = sidText;
    LocalFree(sidText);
    CloseHandle(token);
    return result;
}

} // namespace

std::wstring NativeHostProtocol::WebBridgePipeName()
{
    return L"\\\\.\\pipe\\StatusLight.WebBridge." + CurrentUserSid();
}

std::wstring NativeHostProtocol::Utf8ToWide(const std::string& value)
{
    if (value.empty()) {
        return std::wstring();
    }

    const int requiredSize = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (requiredSize <= 0) {
        return std::wstring();
    }

    std::wstring result(static_cast<size_t>(requiredSize), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), requiredSize);
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}

std::string NativeHostProtocol::WideToUtf8(const std::wstring& value)
{
    if (value.empty()) {
        return std::string();
    }

    const int requiredSize = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (requiredSize <= 0) {
        return std::string();
    }

    std::string result(static_cast<size_t>(requiredSize), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], requiredSize, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

std::string NativeHostProtocol::JsonEscape(const std::string& value)
{
    std::ostringstream output;
    for (char item : value) {
        switch (item) {
        case '\\':
            output << "\\\\";
            break;
        case '"':
            output << "\\\"";
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
            output << item;
            break;
        }
    }
    return output.str();
}

std::string NativeHostProtocol::MakeMessage(const std::string& type)
{
    return "{\"protocolVersion\":1,\"type\":\"" + JsonEscape(type) + "\"}";
}

std::string NativeHostProtocol::MakeBridgeStatus(bool desktopConnected)
{
    return std::string("{\"protocolVersion\":1,\"type\":\"bridge_status\",\"desktopConnected\":") +
        (desktopConnected ? "true" : "false") +
        "}";
}

std::string NativeHostProtocol::MakeProtocolError(const std::string& reason)
{
    return "{\"protocolVersion\":1,\"type\":\"protocol_error\",\"reason\":\"" + JsonEscape(reason) + "\"}";
}

bool NativeHostProtocol::IsAllowedExtensionOrigin(const std::wstring& value)
{
    return value == kAllowedExtensionOrigin;
}

bool NativeHostProtocol::ReadFramedMessage(HANDLE input, std::string* message, std::wstring* errorMessage)
{
    message->clear();

    uint32_t messageSize = 0;
    if (!ReadExact(input, &messageSize, sizeof(messageSize))) {
        if (errorMessage != nullptr) {
            *errorMessage = LastWindowsError();
        }
        return false;
    }

    if (messageSize == 0 || messageSize > kMaxMessageBytes) {
        if (errorMessage != nullptr) {
            *errorMessage = L"message size is outside the allowed range";
        }
        return false;
    }

    message->resize(messageSize);
    if (!ReadExact(input, &(*message)[0], messageSize)) {
        if (errorMessage != nullptr) {
            *errorMessage = LastWindowsError();
        }
        message->clear();
        return false;
    }

    return true;
}

bool NativeHostProtocol::WriteFramedMessage(HANDLE output, const std::string& message, std::wstring* errorMessage)
{
    if (message.empty() || message.size() > kMaxMessageBytes) {
        if (errorMessage != nullptr) {
            *errorMessage = L"message size is outside the allowed range";
        }
        return false;
    }

    const uint32_t messageSize = static_cast<uint32_t>(message.size());
    if (!WriteExact(output, &messageSize, sizeof(messageSize)) ||
        !WriteExact(output, message.data(), messageSize)) {
        if (errorMessage != nullptr) {
            *errorMessage = LastWindowsError();
        }
        return false;
    }

    return true;
}
