/**
 * 文件作用：实现烟花爆炸音效播放器
 * 职责范围：
 * 1. 从 exe 内嵌资源提取烟花 MP3 临时缓存
 * 2. 用后台线程调用 MCI 打开并异步播放 MP3 音效
 * 3. 记录最近一次播放失败原因供诊断使用
 *
 * 不负责：
 * - 音频频谱分析
 * - 视觉粒子生成
 * - 打包或复制音频资源
 *
 * 维护说明：
 * - MCI 不能直接播放内存中的 MP3，因此运行时只在临时目录生成缓存文件。
 */
#include "FireworkAudioPlayer.h"

#include "../../resources/resource.h"

#include <mmsystem.h>

namespace {

const wchar_t* kAudioAlias = L"StatusLightFireworkExplosion";
const wchar_t* kImpactAudioFile = L"firework_explosion_fizz_005.mp3";
const wchar_t* kCrackleAudioFile = L"firework_explosion_fizz_002.mp3";
constexpr size_t kMaxOpenAliases = 12;
constexpr size_t kMaxPendingProfiles = 16;

std::wstring MciErrorText(MCIERROR error)
{
    wchar_t buffer[256] {};
    if (mciGetErrorStringW(error, buffer, static_cast<UINT>(_countof(buffer))) == FALSE) {
        return L"MCI error " + std::to_wstring(static_cast<unsigned long>(error));
    }
    return buffer;
}

int AudioResourceId(FireworkAudioProfile profile)
{
    return profile == FireworkAudioProfile::Crackle002 ?
        IDR_FIREWORK_AUDIO_CRACKLE_002 :
        IDR_FIREWORK_AUDIO_IMPACT_005;
}

const wchar_t* AudioFileName(FireworkAudioProfile profile)
{
    return profile == FireworkAudioProfile::Crackle002 ?
        kCrackleAudioFile :
        kImpactAudioFile;
}

} // namespace

bool FireworkAudioPlayer::Initialize()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return true;
    }

    initialized_ = true;
    shuttingDown_ = false;
    lastError_ = L"None";
    worker_ = std::thread(&FireworkAudioPlayer::WorkerLoop, this);
    return true;
}

void FireworkAudioPlayer::PlayExplosion(FireworkAudioProfile profile)
{
    if (!initialized_ && !Initialize()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingProfiles_.size() >= kMaxPendingProfiles) {
            pendingProfiles_.pop_front();
        }
        pendingProfiles_.push_back(profile);
    }
    wakeWorker_.notify_one();
}

void FireworkAudioPlayer::WorkerLoop()
{
    for (;;) {
        FireworkAudioProfile profile = FireworkAudioProfile::Impact005;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wakeWorker_.wait(lock, [this]() {
                return shuttingDown_ || !pendingProfiles_.empty();
            });

            if (shuttingDown_ && pendingProfiles_.empty()) {
                break;
            }

            profile = pendingProfiles_.front();
            pendingProfiles_.pop_front();
        }

        PlayExplosionOnWorker(profile);
    }
}

void FireworkAudioPlayer::PlayExplosionOnWorker(FireworkAudioProfile profile)
{
    const std::wstring audioPath = EnsureResourceCacheFile(profile);
    if (audioPath.empty()) {
        return;
    }

    TrimOpenAliases();
    const std::wstring alias = std::wstring(kAudioAlias) + std::to_wstring(nextAliasId_++);
    const std::wstring openCommand =
        L"open \"" + audioPath + L"\" type mpegvideo alias " + alias;
    if (!SendMciCommand(openCommand)) {
        return;
    }

    openAliases_.push_back(alias);
    SendMciCommand(L"play " + alias);
}

void FireworkAudioPlayer::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shuttingDown_ = true;
        pendingProfiles_.clear();
    }
    wakeWorker_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }

    for (const std::wstring& alias : openAliases_) {
        SendMciCommand(L"close " + alias);
    }
    openAliases_.clear();
    initialized_ = false;
}

std::wstring FireworkAudioPlayer::LastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

std::wstring FireworkAudioPlayer::ResourceCachePath(FireworkAudioProfile profile) const
{
    wchar_t tempPath[MAX_PATH] {};
    const DWORD tempPathLength = GetTempPathW(static_cast<DWORD>(_countof(tempPath)), tempPath);
    if (tempPathLength == 0 || tempPathLength >= _countof(tempPath)) {
        return std::wstring();
    }

    return std::wstring(tempPath) + L"StatusLight\\audio\\" + AudioFileName(profile);
}

std::wstring FireworkAudioPlayer::EnsureResourceCacheFile(FireworkAudioProfile profile)
{
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(AudioResourceId(profile)), RT_RCDATA);
    if (resource == nullptr) {
        SetLastError(L"AudioResourceMissing");
        return std::wstring();
    }

    const DWORD resourceSize = SizeofResource(nullptr, resource);
    HGLOBAL loadedResource = LoadResource(nullptr, resource);
    const void* resourceBytes = loadedResource == nullptr ? nullptr : LockResource(loadedResource);
    if (resourceSize == 0 || resourceBytes == nullptr) {
        SetLastError(L"AudioResourceLoadFailed");
        return std::wstring();
    }

    const std::wstring audioPath = ResourceCachePath(profile);
    if (audioPath.empty()) {
        SetLastError(L"AudioCachePathUnavailable");
        return std::wstring();
    }

    wchar_t tempPath[MAX_PATH] {};
    const DWORD tempPathLength = GetTempPathW(static_cast<DWORD>(_countof(tempPath)), tempPath);
    if (tempPathLength == 0 || tempPathLength >= _countof(tempPath)) {
        SetLastError(L"AudioCachePathUnavailable");
        return std::wstring();
    }

    const std::wstring rootDirectory = std::wstring(tempPath) + L"StatusLight";
    const std::wstring audioDirectory = rootDirectory + L"\\audio";
    CreateDirectoryW(rootDirectory.c_str(), nullptr);
    CreateDirectoryW(audioDirectory.c_str(), nullptr);

    WIN32_FILE_ATTRIBUTE_DATA fileInfo {};
    const BOOL hasExistingFile = GetFileAttributesExW(audioPath.c_str(), GetFileExInfoStandard, &fileInfo);
    const uint64_t existingSize =
        (static_cast<uint64_t>(fileInfo.nFileSizeHigh) << 32) |
        static_cast<uint64_t>(fileInfo.nFileSizeLow);
    if (hasExistingFile != FALSE && existingSize == resourceSize) {
        return audioPath;
    }

    HANDLE file = CreateFileW(
        audioPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetLastError(L"AudioCacheCreateFailed");
        return std::wstring();
    }

    DWORD written = 0;
    const BOOL wroteFile = WriteFile(file, resourceBytes, resourceSize, &written, nullptr);
    CloseHandle(file);
    if (wroteFile == FALSE || written != resourceSize) {
        DeleteFileW(audioPath.c_str());
        SetLastError(L"AudioCacheWriteFailed");
        return std::wstring();
    }

    return audioPath;
}

bool FireworkAudioPlayer::SendMciCommand(const std::wstring& command)
{
    const MCIERROR error = mciSendStringW(command.c_str(), nullptr, 0, nullptr);
    if (error == 0) {
        SetLastError(L"None");
        return true;
    }

    SetLastError(MciErrorText(error));
    return false;
}

void FireworkAudioPlayer::SetLastError(const std::wstring& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = error;
}

void FireworkAudioPlayer::TrimOpenAliases()
{
    while (openAliases_.size() >= kMaxOpenAliases) {
        SendMciCommand(L"close " + openAliases_.front());
        openAliases_.erase(openAliases_.begin());
    }
}
