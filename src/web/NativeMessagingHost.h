/**
 * 文件作用：声明 StatusLight.exe 的 Chrome Native Messaging Host 模式
 * 职责范围：
 * 1. 校验 Chrome 传入的固定扩展来源
 * 2. 通过 stdin/stdout 处理 Native Messaging 长度帧
 * 3. 连接主进程 WebBridge Named Pipe 并转发消息
 *
 * 不负责：
 * - 直接维护托盘状态
 * - 启动主进程或 Chrome
 *
 * 维护说明：
 * - Chrome 启动的 Host 子进程只做桥接，主进程未运行时最多等待 30 秒后退出
 */
#pragma once

#include <Windows.h>

#include <string>

class NativeMessagingHost {
public:
    static bool LooksLikeNativeHostInvocation(int argc, wchar_t* argv[]);
    static bool IsNativeHostInvocation(int argc, wchar_t* argv[]);
    int Run(int argc, wchar_t* argv[]);

private:
    bool ConnectToBridge(HANDLE* pipe);
    bool ForwardOneMessage(HANDLE pipe);
};
