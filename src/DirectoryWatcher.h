/**
 * 文件作用：声明 Codex sessions 目录监听器
 * 职责范围：
 * 1. 使用 ReadDirectoryChangesW 递归监听 sessions 目录变化
 * 2. 将文件变化转换为主线程可消费的轻量信号
 * 3. 暴露监听失败状态，供托盘层定时恢复
 *
 * 不负责：
 * - 解析 JSONL 文件内容
 * - 聚合任务状态或更新托盘图标
 *
 * 维护说明：
 * - 监听线程只发信号，不直接操作 UI，所有界面更新必须回到主线程处理
 */
#pragma once

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class DirectoryWatcher {
public:
    DirectoryWatcher();
    ~DirectoryWatcher();

    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

    bool Start(const std::wstring& directoryPath, HWND notifyWindow, UINT notifyMessage);
    void Stop();
    bool IsRunning() const;
    bool ConsumeChangeSignal();
    std::wstring LastError() const;

private:
    void ThreadMain();
    void SetLastErrorMessage(const std::wstring& message);

    std::wstring directoryPath_;
    HWND notifyWindow_ = nullptr;
    UINT notifyMessage_ = 0;
    std::thread workerThread_;
    HANDLE stopEvent_ = nullptr;
    HANDLE directoryHandle_ = INVALID_HANDLE_VALUE;
    mutable std::mutex errorMutex_;
    std::wstring lastError_;
    std::atomic<bool> stopRequested_;
    std::atomic<bool> isRunning_;
    std::atomic<bool> hasChanges_;
};
