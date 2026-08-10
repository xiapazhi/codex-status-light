/**
 * 文件作用：声明 StatusLight 的 Gitee Releases 静默自动更新能力。
 * 职责范围：检查版本、下载待更新 EXE、记录更新状态、执行自替换 helper 流程。
 * 不负责：托盘图标业务状态、浏览器监听协议或安装器流程。
 * 维护说明：更新器必须保持无打扰；失败只记录日志，不影响主程序运行。
 */
#pragma once

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct UpdateStatus {
    bool checkInProgress = false;
    bool downloadInProgress = false;
    bool updateReady = false;
    int downloadPercent = 0;
    std::wstring currentVersion;
    std::wstring latestTag;
    std::wstring pendingVersion;
    std::wstring lastRequestUrl;
    std::wstring lastCheckTime;
    std::wstring lastMessage;
    std::wstring readyPath;
};

class AutoUpdater {
public:
    AutoUpdater();
    ~AutoUpdater();

    AutoUpdater(const AutoUpdater&) = delete;
    AutoUpdater& operator=(const AutoUpdater&) = delete;

    void StartBackgroundCheck(bool force);
    void Shutdown();
    UpdateStatus CurrentStatus() const;
    std::wstring BuildDiagnostics() const;
    bool LaunchApplyHelperForCurrentProcess() const;

    static bool HasReadyUpdate();
    static bool LaunchStartupApplyHelperIfReady();
    static int RunCheckUpdateCommand();
    static int RunApplyUpdateCommand(const std::wstring& readyPath, const std::wstring& targetPath, DWORD parentPid);
    static std::wstring ReadyUpdatePath();
    static std::wstring CurrentExecutablePath();

private:
    struct ReleaseInfo {
        std::string tagName;
        std::string exeUrl;
        std::string sha256Url;
        size_t exeSize = 0;
    };

    void CheckWorker(bool force);
    bool CheckOnce(bool allowDownload, std::wstring* message);
    bool FetchLatestRelease(ReleaseInfo* release, std::wstring* message);
    bool DownloadReleaseAsset(const ReleaseInfo& release, std::wstring* message);
    void SetStatus(const UpdateStatus& status);
    void UpdateLastMessage(const std::wstring& message);
    void UpdateDownloadProgress(int percent, const std::wstring& message);
    void UpdateLastRequestUrl(const std::string& url);

    bool DownloadUrl(const std::string& url, size_t maxBytes, std::string* body, std::wstring* message);
    bool DownloadUrlToFile(const std::string& url, const std::wstring& path, size_t expectedSize, std::wstring* message);
    static bool ParseLatestRelease(const std::string& body, ReleaseInfo* release, std::wstring* message);
    static bool IsRemoteVersionNewer(const std::string& tagName);
    static bool IsValidPortableExe(const std::wstring& path);
    bool VerifySha256IfAvailable(const std::wstring& path, const std::string& sha256Url, std::wstring* message);
    static bool ComputeSha256(const std::wstring& path, std::string* digestHex, std::wstring* message);
    static bool EnsureDirectory(const std::wstring& path);
    static std::wstring UpdateDirectory();
    static std::wstring DownloadPath();
    static std::wstring LocalTimeText();
    static std::wstring Widen(const std::string& value);
    static std::string NormalizeVersion(const std::string& tagName);

    mutable std::mutex mutex_;
    UpdateStatus status_;
    std::thread worker_;
    std::atomic<bool> shuttingDown_ { false };
};
