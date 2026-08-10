/**
 * 文件作用：实现基于 Gitee Releases 的无打扰自动更新。
 * 职责范围：请求 latest release、下载 StatusLight.exe、校验文件、缓存待应用更新并执行 helper 替换。
 * 不负责：弹窗通知、安装器生成或私有 Gitee 仓库鉴权。
 * 维护说明：主程序运行期间不得覆盖自身；所有失败都应保留旧版本继续运行。
 */
#include "AutoUpdater.h"

#include "../AppVersion.h"
#include "../JsonValue.h"
#include "../web/WebDebugLog.h"

#include <Wincrypt.h>
#include <Wininet.h>
#include <Shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace {

constexpr const char* kLatestReleaseUrl =
    "https://gitee.com/api/v5/repos/yuan_yi/codex-status-light/releases/latest";
constexpr const wchar_t* kComponent = L"Updater";
constexpr size_t kMaxReleaseJsonBytes = 1024 * 1024;
constexpr size_t kMaxSha256Bytes = 4096;
constexpr size_t kMaxExeAssetBytes = 100 * 1024 * 1024;
constexpr DWORD kApplyWaitTimeoutMs = 30000;

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

std::wstring ParentPath(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return std::wstring();
    }
    return path.substr(0, slash);
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char item) {
        return static_cast<char>(std::tolower(item));
    });
    return value;
}

bool ReadStringField(const JsonValue& object, const std::string& fieldName, std::string* output)
{
    const JsonValue* field = object.GetObjectField(fieldName);
    if (field == nullptr || !field->IsString()) {
        return false;
    }
    *output = field->stringValue;
    return true;
}

size_t ReadSizeFieldOrZero(const JsonValue& object, const std::string& fieldName)
{
    const JsonValue* field = object.GetObjectField(fieldName);
    if (field == nullptr || !field->IsNumber() || field->numberValue < 0) {
        return 0;
    }
    return static_cast<size_t>(field->numberValue);
}

} // namespace

AutoUpdater::AutoUpdater()
{
    status_.currentVersion = AppVersion::kStatusLightVersionWide;
    status_.readyPath = ReadyUpdatePath();
    status_.updateReady = HasReadyUpdate();
}

AutoUpdater::~AutoUpdater()
{
    Shutdown();
}

void AutoUpdater::StartBackgroundCheck(bool force)
{
    if (shuttingDown_) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.checkInProgress) {
            return;
        }
        status_.checkInProgress = true;
        status_.lastMessage = force ? L"manual check queued" : L"background check queued";
    }

    if (worker_.joinable()) {
        worker_.join();
    }
    worker_ = std::thread(&AutoUpdater::CheckWorker, this, force);
}

void AutoUpdater::Shutdown()
{
    shuttingDown_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
}

UpdateStatus AutoUpdater::CurrentStatus() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    UpdateStatus status = status_;
    status.updateReady = HasReadyUpdate();
    return status;
}

std::wstring AutoUpdater::BuildDiagnostics() const
{
    const UpdateStatus status = CurrentStatus();
    std::wostringstream output;
    output << L"Updater current version: " << status.currentVersion << L"\n";
    output << L"Updater latest tag: " << (status.latestTag.empty() ? L"-" : status.latestTag) << L"\n";
    output << L"Updater pending version: " << (status.pendingVersion.empty() ? L"-" : status.pendingVersion) << L"\n";
    output << L"Updater last request url: " << (status.lastRequestUrl.empty() ? L"-" : status.lastRequestUrl) << L"\n";
    output << L"Updater ready: " << (status.updateReady ? L"yes" : L"no") << L"\n";
    output << L"Updater checking: " << (status.checkInProgress ? L"yes" : L"no") << L"\n";
    output << L"Updater downloading: " << (status.downloadInProgress ? L"yes" : L"no") << L"\n";
    output << L"Updater download percent: " << status.downloadPercent << L"%\n";
    output << L"Updater last check: " << (status.lastCheckTime.empty() ? L"-" : status.lastCheckTime) << L"\n";
    output << L"Updater last message: " << (status.lastMessage.empty() ? L"-" : status.lastMessage) << L"\n";
    return output.str();
}

bool AutoUpdater::LaunchApplyHelperForCurrentProcess() const
{
    if (!HasReadyUpdate()) {
        return false;
    }

    const std::wstring exePath = CurrentExecutablePath();
    const std::wstring commandLine =
        L"\"" + exePath + L"\" --apply-update \"" + ReadyUpdatePath() + L"\" \"" +
        exePath + L"\" " + std::to_wstring(GetCurrentProcessId());

    STARTUPINFOW startupInfo {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo {};
    std::vector<wchar_t> command(commandLine.begin(), commandLine.end());
    command.push_back(L'\0');

    const BOOL created = CreateProcessW(
        exePath.c_str(),
        command.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);
    if (!created) {
        WebDebugLog::Write(kComponent, L"failed launch apply helper");
        return false;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    WebDebugLog::Write(kComponent, L"launched apply helper");
    return true;
}

bool AutoUpdater::HasReadyUpdate()
{
    return GetFileAttributesW(ReadyUpdatePath().c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool AutoUpdater::LaunchStartupApplyHelperIfReady()
{
    AutoUpdater updater;
    return updater.LaunchApplyHelperForCurrentProcess();
}

int AutoUpdater::RunCheckUpdateCommand()
{
    AutoUpdater updater;
    std::wstring message;
    const bool ok = updater.CheckOnce(true, &message);
    std::wcout << updater.BuildDiagnostics();
    if (!message.empty()) {
        std::wcout << L"Result: " << message << L"\n";
    }
    return ok ? 0 : 1;
}

int AutoUpdater::RunApplyUpdateCommand(
    const std::wstring& readyPath,
    const std::wstring& targetPath,
    DWORD parentPid)
{
    if (readyPath.empty() || targetPath.empty() || parentPid == 0) {
        WebDebugLog::Write(kComponent, L"apply failed invalid arguments");
        return 2;
    }

    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (parent != nullptr) {
        WaitForSingleObject(parent, kApplyWaitTimeoutMs);
        CloseHandle(parent);
    }

    if (!IsValidPortableExe(readyPath)) {
        WebDebugLog::Write(kComponent, L"apply failed ready file is not valid exe");
        return 1;
    }

    const std::wstring backupPath = targetPath + L".old";
    DeleteFileW(backupPath.c_str());
    MoveFileExW(targetPath.c_str(), backupPath.c_str(), MOVEFILE_REPLACE_EXISTING);

    const BOOL moved = MoveFileExW(
        readyPath.c_str(),
        targetPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!moved) {
        MoveFileExW(backupPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        WebDebugLog::Write(kComponent, L"apply failed replace target");
        return 1;
    }

    WebDebugLog::Write(kComponent, L"apply ok restarting target");
    ShellExecuteW(nullptr, L"open", targetPath.c_str(), nullptr, ParentPath(targetPath).c_str(), SW_SHOWNORMAL);
    return 0;
}

std::wstring AutoUpdater::ReadyUpdatePath()
{
    return JoinPath(UpdateDirectory(), L"StatusLight.exe.ready");
}

std::wstring AutoUpdater::CurrentExecutablePath()
{
    wchar_t buffer[MAX_PATH] {};
    const DWORD size = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(_countof(buffer)));
    if (size == 0 || size >= _countof(buffer)) {
        return std::wstring();
    }
    return buffer;
}

void AutoUpdater::CheckWorker(bool force)
{
    std::wstring message;
    CheckOnce(true, &message);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.checkInProgress = false;
        if (!message.empty()) {
            status_.lastMessage = message;
        } else if (force) {
            status_.lastMessage = L"manual check completed";
        } else {
            status_.lastMessage = L"background check completed";
        }
        status_.lastCheckTime = LocalTimeText();
        status_.updateReady = HasReadyUpdate();
    }
}

bool AutoUpdater::CheckOnce(bool allowDownload, std::wstring* message)
{
    UpdateStatus newStatus = CurrentStatus();
    newStatus.lastCheckTime = LocalTimeText();
    newStatus.lastMessage = L"checking latest release";
    newStatus.downloadInProgress = false;
    newStatus.downloadPercent = 0;
    SetStatus(newStatus);

    ReleaseInfo release;
    if (!FetchLatestRelease(&release, message)) {
        if (message != nullptr && !message->empty()) {
            UpdateLastMessage(*message);
        } else {
            UpdateLastMessage(L"failed to fetch latest release");
        }
        return false;
    }

    newStatus = CurrentStatus();
    newStatus.latestTag = Widen(release.tagName);
    newStatus.pendingVersion.clear();
    newStatus.lastMessage = L"latest release fetched";
    SetStatus(newStatus);

    if (!IsRemoteVersionNewer(release.tagName)) {
        if (message != nullptr) {
            *message = L"already up to date";
        }
        UpdateLastMessage(L"already up to date");
        return true;
    }

    if (!allowDownload) {
        if (message != nullptr) {
            *message = L"new version available";
        }
        UpdateLastMessage(L"new version available");
        return true;
    }

    if (!DownloadReleaseAsset(release, message)) {
        if (message != nullptr && !message->empty()) {
            UpdateLastMessage(*message);
        } else {
            UpdateLastMessage(L"failed to download update");
        }
        return false;
    }

    newStatus = CurrentStatus();
    newStatus.pendingVersion = Widen(release.tagName);
    newStatus.updateReady = true;
    newStatus.downloadInProgress = false;
    newStatus.downloadPercent = 100;
    newStatus.lastMessage = L"update downloaded and ready";
    SetStatus(newStatus);
    WebDebugLog::Write(kComponent, L"downloaded ready version=" + Widen(release.tagName));
    return true;
}

bool AutoUpdater::FetchLatestRelease(ReleaseInfo* release, std::wstring* message)
{
    std::string body;
    if (!DownloadUrl(kLatestReleaseUrl, kMaxReleaseJsonBytes, &body, message)) {
        return false;
    }
    return ParseLatestRelease(body, release, message);
}

bool AutoUpdater::DownloadReleaseAsset(const ReleaseInfo& release, std::wstring* message)
{
    if (release.exeUrl.empty()) {
        if (message != nullptr) {
            *message = L"release asset StatusLight.exe not found";
        }
        return false;
    }

    if (!EnsureDirectory(UpdateDirectory())) {
        if (message != nullptr) {
            *message = L"failed to create update directory";
        }
        return false;
    }

    const std::wstring downloadPath = DownloadPath();
    const std::wstring readyPath = ReadyUpdatePath();
    DeleteFileW(downloadPath.c_str());
    UpdateDownloadProgress(0, L"downloading update");

    if (!DownloadUrlToFile(release.exeUrl, downloadPath, release.exeSize, message)) {
        DeleteFileW(downloadPath.c_str());
        return false;
    }

    if (!IsValidPortableExe(downloadPath)) {
        DeleteFileW(downloadPath.c_str());
        if (message != nullptr) {
            *message = L"downloaded file is not a valid exe";
        }
        WebDebugLog::Write(kComponent, L"failed downloaded file invalid exe");
        return false;
    }

    if (!VerifySha256IfAvailable(downloadPath, release.sha256Url, message)) {
        DeleteFileW(downloadPath.c_str());
        return false;
    }

    DeleteFileW(readyPath.c_str());
    if (!MoveFileExW(downloadPath.c_str(), readyPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(downloadPath.c_str());
        if (message != nullptr) {
            *message = L"failed to mark update ready";
        }
        return false;
    }

    return true;
}

void AutoUpdater::SetStatus(const UpdateStatus& status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = status;
    status_.currentVersion = AppVersion::kStatusLightVersionWide;
    status_.readyPath = ReadyUpdatePath();
    status_.updateReady = HasReadyUpdate();
}

void AutoUpdater::UpdateLastMessage(const std::wstring& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_.lastMessage = message;
    status_.lastCheckTime = LocalTimeText();
    status_.downloadInProgress = false;
    status_.downloadPercent = 0;
    status_.updateReady = HasReadyUpdate();
}

void AutoUpdater::UpdateDownloadProgress(int percent, const std::wstring& message)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    status_.downloadInProgress = true;
    status_.downloadPercent = percent;
    status_.lastMessage = message;
    status_.lastCheckTime = LocalTimeText();
    status_.updateReady = HasReadyUpdate();
}

void AutoUpdater::UpdateLastRequestUrl(const std::string& url)
{
    const std::wstring wideUrl = Widen(url);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.lastRequestUrl = wideUrl;
        status_.lastCheckTime = LocalTimeText();
    }
    WebDebugLog::Write(kComponent, L"request url=" + wideUrl);
}

bool AutoUpdater::DownloadUrl(
    const std::string& url,
    size_t maxBytes,
    std::string* body,
    std::wstring* message)
{
    UpdateLastRequestUrl(url);

    HINTERNET internet = InternetOpenW(
        L"StatusLight updater",
        INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr,
        nullptr,
        0);
    if (internet == nullptr) {
        if (message != nullptr) {
            *message = L"failed to initialize WinINet";
        }
        return false;
    }
    DWORD timeoutMs = 15000;
    InternetSetOptionW(internet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionW(internet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionW(internet, INTERNET_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

    const std::wstring wideUrl = Widen(url);
    const wchar_t* headers =
        L"User-Agent: StatusLight\r\n"
        L"Accept: application/json, application/octet-stream\r\n";
    HINTERNET request = InternetOpenUrlW(
        internet,
        wideUrl.c_str(),
        headers,
        static_cast<DWORD>(wcslen(headers)),
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE,
        0);
    if (request == nullptr) {
        InternetCloseHandle(internet);
        if (message != nullptr) {
            *message = L"failed to open update url";
        }
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusCodeSize, nullptr) &&
        (statusCode < 200 || statusCode >= 300)) {
        InternetCloseHandle(request);
        InternetCloseHandle(internet);
        if (message != nullptr) {
            std::wostringstream output;
            output << L"update url returned HTTP " << statusCode;
            *message = output.str();
        }
        return false;
    }

    body->clear();
    char buffer[8192] {};
    DWORD bytesRead = 0;
    while (InternetReadFile(request, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        if (body->size() + bytesRead > maxBytes) {
            InternetCloseHandle(request);
            InternetCloseHandle(internet);
            if (message != nullptr) {
                *message = L"downloaded response too large";
            }
            return false;
        }
        body->append(buffer, bytesRead);
    }

    InternetCloseHandle(request);
    InternetCloseHandle(internet);
    return true;
}

bool AutoUpdater::DownloadUrlToFile(
    const std::string& url,
    const std::wstring& path,
    size_t expectedSize,
    std::wstring* message)
{
    UpdateLastRequestUrl(url);

    if (expectedSize > kMaxExeAssetBytes) {
        if (message != nullptr) {
            *message = L"release asset is too large";
        }
        return false;
    }

    HINTERNET internet = InternetOpenW(
        L"StatusLight updater",
        INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr,
        nullptr,
        0);
    if (internet == nullptr) {
        if (message != nullptr) {
            *message = L"failed to initialize WinINet";
        }
        return false;
    }

    DWORD timeoutMs = 15000;
    InternetSetOptionW(internet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionW(internet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionW(internet, INTERNET_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

    const std::wstring wideUrl = Widen(url);
    const wchar_t* headers =
        L"User-Agent: StatusLight\r\n"
        L"Accept: application/octet-stream\r\n";
    HINTERNET request = InternetOpenUrlW(
        internet,
        wideUrl.c_str(),
        headers,
        static_cast<DWORD>(wcslen(headers)),
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE,
        0);
    if (request == nullptr) {
        InternetCloseHandle(internet);
        if (message != nullptr) {
            *message = L"failed to open update asset url";
        }
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusCodeSize, nullptr) &&
        (statusCode < 200 || statusCode >= 300)) {
        InternetCloseHandle(request);
        InternetCloseHandle(internet);
        if (message != nullptr) {
            std::wostringstream output;
            output << L"update asset returned HTTP " << statusCode;
            *message = output.str();
        }
        return false;
    }

    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        InternetCloseHandle(request);
        InternetCloseHandle(internet);
        if (message != nullptr) {
            *message = L"failed to create download file";
        }
        return false;
    }

    size_t totalBytes = 0;
    bool ok = true;
    int lastPercent = -1;
    char buffer[8192] {};
    DWORD bytesRead = 0;
    BOOL readSucceeded = FALSE;
    for (;;) {
        readSucceeded = InternetReadFile(request, buffer, sizeof(buffer), &bytesRead);
        if (!readSucceeded || bytesRead == 0) {
            break;
        }

        totalBytes += bytesRead;
        if (totalBytes > kMaxExeAssetBytes) {
            ok = false;
            if (message != nullptr) {
                *message = L"downloaded file too large";
            }
            break;
        }

        DWORD bytesWritten = 0;
        if (!WriteFile(file, buffer, bytesRead, &bytesWritten, nullptr) || bytesWritten != bytesRead) {
            ok = false;
            if (message != nullptr) {
                *message = L"failed to write complete download file";
            }
            break;
        }

        if (expectedSize > 0) {
            const int percent = static_cast<int>(std::min<size_t>(100, totalBytes * 100 / expectedSize));
            if (percent != lastPercent) {
                lastPercent = percent;
                std::wostringstream progress;
                progress << L"downloading update " << percent << L"%";
                UpdateDownloadProgress(percent, progress.str());
            }
        }
    }

    CloseHandle(file);
    InternetCloseHandle(request);
    InternetCloseHandle(internet);

    if (!readSucceeded) {
        DeleteFileW(path.c_str());
        if (message != nullptr) {
            *message = L"failed to read update asset";
        }
        return false;
    }
    if (!ok) {
        DeleteFileW(path.c_str());
        return false;
    }
    if (expectedSize > 0 && totalBytes != expectedSize) {
        DeleteFileW(path.c_str());
        if (message != nullptr) {
            *message = L"downloaded file size mismatch";
        }
        return false;
    }
    return true;
}

bool AutoUpdater::ParseLatestRelease(const std::string& body, ReleaseInfo* release, std::wstring* message)
{
    JsonValue root;
    JsonParser parser;
    std::string error;
    if (!parser.Parse(body, &root, &error) || !root.IsObject()) {
        if (message != nullptr) {
            *message = L"failed to parse release json";
        }
        WebDebugLog::WriteUtf8(kComponent, "failed parse release json error=" + error);
        return false;
    }

    if (!ReadStringField(root, "tag_name", &release->tagName) || release->tagName.empty()) {
        if (message != nullptr) {
            *message = L"release tag_name not found";
        }
        return false;
    }

    const JsonValue* assets = root.GetObjectField("assets");
    if (assets == nullptr || !assets->IsArray()) {
        if (message != nullptr) {
            *message = L"release assets not found";
        }
        return false;
    }

    for (const JsonValue& asset : assets->arrayValue) {
        if (!asset.IsObject()) {
            continue;
        }

        std::string name;
        std::string url;
        if (!ReadStringField(asset, "name", &name) ||
            !ReadStringField(asset, "browser_download_url", &url)) {
            continue;
        }

        const std::string lowerName = LowerAscii(name);
        if (lowerName == "statuslight.exe") {
            release->exeUrl = url;
            release->exeSize = ReadSizeFieldOrZero(asset, "size");
        } else if (lowerName == "statuslight.exe.sha256") {
            release->sha256Url = url;
        }
    }

    if (release->exeUrl.empty()) {
        if (message != nullptr) {
            *message = L"release asset StatusLight.exe not found";
        }
        return false;
    }
    return true;
}

bool AutoUpdater::IsRemoteVersionNewer(const std::string& tagName)
{
    const std::string remote = NormalizeVersion(tagName);
    const std::string local = NormalizeVersion(AppVersion::kStatusLightVersion);
    if (remote.empty() || local.empty()) {
        return remote > local;
    }

    std::istringstream remoteStream(remote);
    std::istringstream localStream(local);
    while (remoteStream.good() || localStream.good()) {
        std::string remotePart;
        std::string localPart;
        std::getline(remoteStream, remotePart, '.');
        std::getline(localStream, localPart, '.');
        const int remoteValue = remotePart.empty() ? 0 : std::atoi(remotePart.c_str());
        const int localValue = localPart.empty() ? 0 : std::atoi(localPart.c_str());
        if (remoteValue != localValue) {
            return remoteValue > localValue;
        }
    }
    return false;
}

bool AutoUpdater::IsValidPortableExe(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    unsigned char dosHeader[64] {};
    DWORD bytesRead = 0;
    if (!ReadFile(file, dosHeader, sizeof(dosHeader), &bytesRead, nullptr) || bytesRead != sizeof(dosHeader)) {
        CloseHandle(file);
        return false;
    }
    if (dosHeader[0] != 'M' || dosHeader[1] != 'Z') {
        CloseHandle(file);
        return false;
    }

    const LONG peOffset = *reinterpret_cast<LONG*>(&dosHeader[0x3c]);
    if (peOffset <= 0) {
        CloseHandle(file);
        return false;
    }

    SetFilePointer(file, peOffset, nullptr, FILE_BEGIN);
    unsigned char peSignature[4] {};
    bytesRead = 0;
    const bool ok = ReadFile(file, peSignature, sizeof(peSignature), &bytesRead, nullptr) &&
        bytesRead == sizeof(peSignature) &&
        peSignature[0] == 'P' &&
        peSignature[1] == 'E' &&
        peSignature[2] == 0 &&
        peSignature[3] == 0;
    CloseHandle(file);
    return ok;
}

bool AutoUpdater::VerifySha256IfAvailable(
    const std::wstring& path,
    const std::string& sha256Url,
    std::wstring* message)
{
    if (sha256Url.empty()) {
        WebDebugLog::Write(kComponent, L"sha256 asset missing, using PE validation only");
        return true;
    }

    std::string expectedBody;
    if (!DownloadUrl(sha256Url, kMaxSha256Bytes, &expectedBody, message)) {
        return false;
    }

    std::string expected;
    for (char item : expectedBody) {
        if (std::isxdigit(static_cast<unsigned char>(item)) != 0) {
            expected.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(item))));
            if (expected.size() == 64) {
                break;
            }
        } else if (!expected.empty()) {
            break;
        }
    }
    if (expected.size() != 64) {
        if (message != nullptr) {
            *message = L"invalid sha256 file";
        }
        return false;
    }

    std::string actual;
    if (!ComputeSha256(path, &actual, message)) {
        return false;
    }
    if (actual != expected) {
        if (message != nullptr) {
            *message = L"sha256 mismatch";
        }
        return false;
    }
    return true;
}

bool AutoUpdater::ComputeSha256(const std::wstring& path, std::string* digestHex, std::wstring* message)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (message != nullptr) {
            *message = L"failed to open file for sha256";
        }
        return false;
    }

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        CloseHandle(file);
        if (provider != 0) {
            CryptReleaseContext(provider, 0);
        }
        if (message != nullptr) {
            *message = L"failed to initialize sha256";
        }
        return false;
    }

    unsigned char buffer[8192] {};
    DWORD bytesRead = 0;
    bool ok = true;
    while (ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        if (!CryptHashData(hash, buffer, bytesRead, 0)) {
            ok = false;
            break;
        }
    }

    unsigned char digest[32] {};
    DWORD digestSize = sizeof(digest);
    if (ok && !CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0)) {
        ok = false;
    }

    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    CloseHandle(file);

    if (!ok) {
        if (message != nullptr) {
            *message = L"failed to compute sha256";
        }
        return false;
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (DWORD index = 0; index < digestSize; ++index) {
        output << std::setw(2) << static_cast<int>(digest[index]);
    }
    *digestHex = output.str();
    return true;
}

bool AutoUpdater::EnsureDirectory(const std::wstring& path)
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

std::wstring AutoUpdater::UpdateDirectory()
{
    wchar_t buffer[MAX_PATH] {};
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(_countof(buffer)));
    if (size == 0 || size >= _countof(buffer)) {
        return ParentPath(CurrentExecutablePath());
    }
    return JoinPath(buffer, L"StatusLight\\updates");
}

std::wstring AutoUpdater::DownloadPath()
{
    return JoinPath(UpdateDirectory(), L"StatusLight.exe.download");
}

std::wstring AutoUpdater::LocalTimeText()
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
        << (time.wSecond < 10 ? L"0" : L"") << time.wSecond;
    return output.str();
}

std::wstring AutoUpdater::Widen(const std::string& value)
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

std::string AutoUpdater::NormalizeVersion(const std::string& tagName)
{
    std::string value = tagName;
    if (!value.empty() && (value[0] == 'v' || value[0] == 'V')) {
        value.erase(value.begin());
    }

    std::string normalized;
    for (char item : value) {
        if ((item >= '0' && item <= '9') || item == '.') {
            normalized.push_back(item);
        } else {
            break;
        }
    }
    return normalized;
}
