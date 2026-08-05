/**
 * 文件作用：实现 StatusLight 主进程的 WebBridge Named Pipe 服务
 * 职责范围：
 * 1. 创建当前用户隔离的 Named Pipe
 * 2. 接收 Native Host 子进程转发的 JSON 消息
 * 3. 将处理结果按同一长度帧协议写回
 *
 * 不负责：
 * - 启动 Chrome 扩展
 * - 保存任何网页正文或账号信息
 *
 * 维护说明：
 * - 管道服务异常只影响 Web 来源，不能影响 Codex 本地监听
 */
#include "WebBridgePipeServer.h"

#include "NativeHostProtocol.h"
#include "WebDebugLog.h"

#include <sstream>

namespace {

std::wstring LastWindowsError()
{
    std::wostringstream output;
    output << L"Windows error " << GetLastError();
    return output.str();
}

} // namespace

WebBridgePipeServer::~WebBridgePipeServer()
{
    Stop();
}

bool WebBridgePipeServer::Start(MessageHandler handler, std::wstring* errorMessage)
{
    if (running_) {
        WebDebugLog::Write(L"PipeServer", L"start ignored already_running=true");
        return true;
    }

    handler_ = handler;
    stopRequested_ = false;
    lastError_.clear();

    try {
        worker_ = std::thread(&WebBridgePipeServer::ServerLoop, this);
    } catch (...) {
        if (errorMessage != nullptr) {
            *errorMessage = L"failed to start WebBridge pipe thread";
        }
        WebDebugLog::Write(L"PipeServer", L"start failed create_thread=false");
        return false;
    }

    running_ = true;
    WebDebugLog::Write(L"PipeServer", L"thread started");
    return true;
}

void WebBridgePipeServer::Stop()
{
    stopRequested_ = true;
    WebDebugLog::Write(L"PipeServer", L"stop requested");
    WakeBlockedServer();

    if (worker_.joinable()) {
        worker_.join();
    }

    running_ = false;
    connectedClientCount_ = 0;
}

bool WebBridgePipeServer::IsRunning() const
{
    return running_ && !stopRequested_;
}

size_t WebBridgePipeServer::ConnectedClientCount() const
{
    return connectedClientCount_;
}

std::wstring WebBridgePipeServer::LastError() const
{
    return lastError_;
}

void WebBridgePipeServer::ServerLoop()
{
    const std::wstring pipeName = NativeHostProtocol::WebBridgePipeName();
    WebDebugLog::Write(L"PipeServer", L"listen pipe=" + pipeName);

    while (!stopRequested_) {
        HANDLE pipe = CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            NativeHostProtocol::kMaxMessageBytes + 4,
            NativeHostProtocol::kMaxMessageBytes + 4,
            0,
            nullptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            lastError_ = LastWindowsError();
            WebDebugLog::Write(L"PipeServer", L"create_named_pipe failed error=" + lastError_);
            Sleep(1000);
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!connected) {
            CloseHandle(pipe);
            if (!stopRequested_) {
                lastError_ = LastWindowsError();
                WebDebugLog::Write(L"PipeServer", L"connect_named_pipe failed error=" + lastError_);
            }
            continue;
        }

        if (stopRequested_) {
            CloseHandle(pipe);
            break;
        }

        ++connectedClientCount_;
        WebDebugLog::Write(L"PipeServer", L"client connected");
        HandleClient(pipe);
        --connectedClientCount_;
        WebDebugLog::Write(L"PipeServer", L"client disconnected");
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    WebDebugLog::Write(L"PipeServer", L"listen loop exited");
}

void WebBridgePipeServer::HandleClient(HANDLE pipe)
{
    while (!stopRequested_) {
        std::string request;
        std::wstring errorMessage;
        if (!NativeHostProtocol::ReadFramedMessage(pipe, &request, &errorMessage)) {
            lastError_ = errorMessage;
            WebDebugLog::Write(L"PipeServer", L"read client failed error=" + errorMessage);
            return;
        }

        std::string response = NativeHostProtocol::MakeProtocolError("handler_unavailable");
        if (handler_) {
            response = handler_(request);
        }

        if (!NativeHostProtocol::WriteFramedMessage(pipe, response, &errorMessage)) {
            lastError_ = errorMessage;
            WebDebugLog::Write(L"PipeServer", L"write client failed error=" + errorMessage);
            return;
        }
    }
}

void WebBridgePipeServer::WakeBlockedServer()
{
    const std::wstring pipeName = NativeHostProtocol::WebBridgePipeName();
    HANDLE pipe = CreateFileW(
        pipeName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
    }
}
