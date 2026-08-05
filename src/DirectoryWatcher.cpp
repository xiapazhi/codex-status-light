/**
 * 文件作用：实现 Codex sessions 目录监听器
 * 职责范围：
 * 1. 使用 Win32 原生 ReadDirectoryChangesW 获取文件变化
 * 2. 处理监听线程退出、取消 IO 和失败记录
 * 3. 通知主线程执行实际状态刷新
 *
 * 不负责：
 * - 决定任务颜色
 * - 读取或保存 Codex JSONL 正文
 *
 * 维护说明：
 * - 监听失败不会让程序崩溃，托盘层会通过 5 秒校准扫描尝试恢复监听
 */
#include "DirectoryWatcher.h"

#include <array>

namespace {

constexpr DWORD kWatchFilters =
    FILE_NOTIFY_CHANGE_FILE_NAME |
    FILE_NOTIFY_CHANGE_DIR_NAME |
    FILE_NOTIFY_CHANGE_SIZE |
    FILE_NOTIFY_CHANGE_LAST_WRITE;

} // namespace

DirectoryWatcher::DirectoryWatcher()
    : stopRequested_(false),
      isRunning_(false),
      hasChanges_(false)
{
}

DirectoryWatcher::~DirectoryWatcher()
{
    Stop();
}

bool DirectoryWatcher::Start(const std::wstring& directoryPath, HWND notifyWindow, UINT notifyMessage)
{
    Stop();

    directoryPath_ = directoryPath;
    notifyWindow_ = notifyWindow;
    notifyMessage_ = notifyMessage;
    stopRequested_ = false;
    hasChanges_ = false;
    SetLastErrorMessage(std::wstring());

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent_ == nullptr) {
        SetLastErrorMessage(L"failed to create watcher stop event");
        return false;
    }

    directoryHandle_ = CreateFileW(
        directoryPath_.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (directoryHandle_ == INVALID_HANDLE_VALUE) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        SetLastErrorMessage(L"failed to open sessions directory for watching");
        return false;
    }

    workerThread_ = std::thread(&DirectoryWatcher::ThreadMain, this);
    return true;
}

void DirectoryWatcher::Stop()
{
    stopRequested_ = true;

    if (stopEvent_ != nullptr) {
        SetEvent(stopEvent_);
    }

    if (directoryHandle_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(directoryHandle_, nullptr);
    }

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    if (directoryHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(directoryHandle_);
        directoryHandle_ = INVALID_HANDLE_VALUE;
    }

    if (stopEvent_ != nullptr) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }

    isRunning_ = false;
}

bool DirectoryWatcher::IsRunning() const
{
    return isRunning_;
}

bool DirectoryWatcher::ConsumeChangeSignal()
{
    return hasChanges_.exchange(false);
}

std::wstring DirectoryWatcher::LastError() const
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

void DirectoryWatcher::ThreadMain()
{
    isRunning_ = true;
    std::array<unsigned char, 64 * 1024> buffer {};

    while (!stopRequested_) {
        HANDLE changeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (changeEvent == nullptr) {
            SetLastErrorMessage(L"failed to create watcher change event");
            break;
        }

        OVERLAPPED overlapped {};
        overlapped.hEvent = changeEvent;
        DWORD bytesReturned = 0;

        const BOOL watchStarted = ReadDirectoryChangesW(
            directoryHandle_,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            TRUE,
            kWatchFilters,
            &bytesReturned,
            &overlapped,
            nullptr);

        if (!watchStarted) {
            CloseHandle(changeEvent);
            SetLastErrorMessage(L"ReadDirectoryChangesW failed to start");
            break;
        }

        HANDLE waitHandles[2] = { stopEvent_, changeEvent };
        const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0) {
            CancelIoEx(directoryHandle_, &overlapped);
            CloseHandle(changeEvent);
            break;
        }

        if (waitResult != WAIT_OBJECT_0 + 1) {
            CancelIoEx(directoryHandle_, &overlapped);
            CloseHandle(changeEvent);
            SetLastErrorMessage(L"watcher wait failed");
            break;
        }

        DWORD transferred = 0;
        const BOOL result = GetOverlappedResult(directoryHandle_, &overlapped, &transferred, FALSE);
        CloseHandle(changeEvent);

        if (!result) {
            if (stopRequested_) {
                break;
            }
            SetLastErrorMessage(L"watcher result failed");
            break;
        }

        hasChanges_ = true;
        if (notifyWindow_ != nullptr && notifyMessage_ != 0) {
            PostMessageW(notifyWindow_, notifyMessage_, 0, 0);
        }
    }

    isRunning_ = false;
}

void DirectoryWatcher::SetLastErrorMessage(const std::wstring& message)
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_ = message;
}
