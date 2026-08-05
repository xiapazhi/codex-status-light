/**
 * 文件作用：声明 StatusLight 主进程的 WebBridge Named Pipe 服务
 * 职责范围：
 * 1. 监听 Native Host 子进程转发来的扩展消息
 * 2. 使用长度帧协议收发 UTF-8 JSON
 * 3. 将消息交给上层 WebSourceController 处理
 *
 * 不负责：
 * - 解析 ChatGPT 状态语义
 * - 直接修改托盘图标
 *
 * 维护说明：
 * - Stop 会主动连接一次管道唤醒阻塞线程，避免退出时卡住
 */
#pragma once

#include <Windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

class WebBridgePipeServer {
public:
    using MessageHandler = std::function<std::string(const std::string&)>;

    WebBridgePipeServer() = default;
    ~WebBridgePipeServer();

    WebBridgePipeServer(const WebBridgePipeServer&) = delete;
    WebBridgePipeServer& operator=(const WebBridgePipeServer&) = delete;

    bool Start(MessageHandler handler, std::wstring* errorMessage);
    void Stop();
    bool IsRunning() const;
    size_t ConnectedClientCount() const;
    std::wstring LastError() const;

private:
    std::atomic<bool> stopRequested_ { false };
    std::atomic<bool> running_ { false };
    std::atomic<size_t> connectedClientCount_ { 0 };
    std::thread worker_;
    MessageHandler handler_;
    std::wstring lastError_;

    void ServerLoop();
    void HandleClient(HANDLE pipe);
    void WakeBlockedServer();
};
