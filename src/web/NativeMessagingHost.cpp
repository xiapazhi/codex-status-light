/**
 * 文件作用：实现 StatusLight.exe 的 Chrome Native Messaging Host 子进程模式
 * 职责范围：
 * 1. 将 Windows stdin/stdout 切换为二进制模式
 * 2. 等待并连接 StatusLight 主进程 Named Pipe
 * 3. 在 Chrome 扩展和主进程之间转发 JSON 长度帧
 *
 * 不负责：
 * - 解析 ChatGPT DOM 状态
 * - 写普通日志到 stdout
 *
 * 维护说明：
 * - stdout 被 Chrome 协议占用，所有错误只能写 stderr
 */
#include "NativeMessagingHost.h"

#include "NativeHostProtocol.h"
#include "WebDebugLog.h"

#include "../JsonValue.h"

#include <Windows.h>
#include <fcntl.h>
#include <io.h>

#include <iostream>
#include <sstream>

namespace {

std::wstring LastWindowsError()
{
    std::wostringstream output;
    output << L"Windows error " << GetLastError();
    return output.str();
}

std::string MessageTypeForLog(const std::string& message)
{
    JsonValue root;
    std::string parseError;
    JsonParser parser;
    if (!parser.Parse(message, &root, &parseError) || !root.IsObject()) {
        return "invalid_json";
    }

    const JsonValue* typeField = root.GetObjectField("type");
    if (typeField == nullptr || !typeField->IsString()) {
        return "missing_type";
    }

    return typeField->stringValue;
}

} // namespace

bool NativeMessagingHost::LooksLikeNativeHostInvocation(int argc, wchar_t* argv[])
{
    if (argc < 2 || argv[1] == nullptr) {
        return false;
    }

    const std::wstring origin = argv[1];
    return origin.rfind(L"chrome-extension://", 0) == 0;
}

bool NativeMessagingHost::IsNativeHostInvocation(int argc, wchar_t* argv[])
{
    if (argc < 2) {
        return false;
    }
    return NativeHostProtocol::IsAllowedExtensionOrigin(argv[1]);
}

int NativeMessagingHost::Run(int argc, wchar_t* argv[])
{
    if (!IsNativeHostInvocation(argc, argv)) {
        const std::wstring origin = argc >= 2 && argv[1] != nullptr ? argv[1] : L"(missing)";
        WebDebugLog::Write(L"NativeHost", L"reject origin=" + origin);
        std::wcerr << L"invalid native messaging origin\n";
        return 2;
    }

    WebDebugLog::Write(L"NativeHost", L"started by Chrome origin=allowed");

    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    WebDebugLog::Write(L"NativeHost", L"stdio binary mode enabled");

    HANDLE pipe = INVALID_HANDLE_VALUE;
    if (!ConnectToBridge(&pipe)) {
        WebDebugLog::Write(L"NativeHost", L"pipe connect failed, exit");
        return 1;
    }

    std::wstring errorMessage;
    NativeHostProtocol::WriteFramedMessage(
        GetStdHandle(STD_OUTPUT_HANDLE),
        NativeHostProtocol::MakeBridgeStatus(true),
        &errorMessage);

    while (ForwardOneMessage(pipe)) {
    }

    WebDebugLog::Write(L"NativeHost", L"forward loop ended");
    CloseHandle(pipe);
    return 0;
}

bool NativeMessagingHost::ConnectToBridge(HANDLE* pipe)
{
    const std::wstring pipeName = NativeHostProtocol::WebBridgePipeName();
    const ULONGLONG startedAt = GetTickCount64();
    ULONGLONG lastStatusAt = 0;

    while (GetTickCount64() - startedAt < 30000) {
        *pipe = CreateFileW(
            pipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (*pipe != INVALID_HANDLE_VALUE) {
            WebDebugLog::Write(L"NativeHost", L"pipe connected");
            return true;
        }

        const ULONGLONG now = GetTickCount64();
        if (lastStatusAt == 0 || now - lastStatusAt >= 1000) {
            std::wstring ignoredError;
            NativeHostProtocol::WriteFramedMessage(
                GetStdHandle(STD_OUTPUT_HANDLE),
                NativeHostProtocol::MakeBridgeStatus(false),
                &ignoredError);
            lastStatusAt = now;
        }

        Sleep(250);
    }

    WebDebugLog::Write(L"NativeHost", L"pipe wait timeout last_error=" + LastWindowsError());
    std::wcerr << L"StatusLight WebBridge pipe is unavailable\n";
    return false;
}

bool NativeMessagingHost::ForwardOneMessage(HANDLE pipe)
{
    std::string message;
    std::wstring errorMessage;
    if (!NativeHostProtocol::ReadFramedMessage(GetStdHandle(STD_INPUT_HANDLE), &message, &errorMessage)) {
        WebDebugLog::Write(L"NativeHost", L"read chrome failed error=" + errorMessage);
        return false;
    }

    WebDebugLog::WriteUtf8(L"NativeHost", "chrome message type=" + MessageTypeForLog(message));
    if (!NativeHostProtocol::WriteFramedMessage(pipe, message, &errorMessage)) {
        WebDebugLog::Write(L"NativeHost", L"write pipe failed error=" + errorMessage);
        NativeHostProtocol::WriteFramedMessage(
            GetStdHandle(STD_OUTPUT_HANDLE),
            NativeHostProtocol::MakeBridgeStatus(false),
            &errorMessage);
        return false;
    }

    std::string response;
    if (!NativeHostProtocol::ReadFramedMessage(pipe, &response, &errorMessage)) {
        WebDebugLog::Write(L"NativeHost", L"read pipe response failed error=" + errorMessage);
        NativeHostProtocol::WriteFramedMessage(
            GetStdHandle(STD_OUTPUT_HANDLE),
            NativeHostProtocol::MakeBridgeStatus(false),
            &errorMessage);
        return false;
    }

    WebDebugLog::WriteUtf8(L"NativeHost", "pipe response type=" + MessageTypeForLog(response));
    return NativeHostProtocol::WriteFramedMessage(GetStdHandle(STD_OUTPUT_HANDLE), response, &errorMessage);
}
