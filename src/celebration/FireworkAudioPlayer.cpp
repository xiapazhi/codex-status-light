/**
 * 文件作用：实现烟花爆炸音效播放器
 * 职责范围：
 * 1. 定位 exe 同级 assets/audio 目录
 * 2. 用 MCI 打开并异步播放 MP3 音效
 * 3. 记录最近一次播放失败原因供诊断使用
 *
 * 不负责：
 * - 音频频谱分析
 * - 视觉粒子生成
 * - 打包或复制音频资源
 *
 * 维护说明：
 * - MCI 命令字符串对引号敏感，文件路径必须用双引号包裹。
 */
#include "FireworkAudioPlayer.h"

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

} // namespace

bool FireworkAudioPlayer::Initialize()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return true;
    }

    wchar_t modulePath[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(_countof(modulePath)));
    if (length == 0 || length >= _countof(modulePath)) {
        lastError_ = L"GetModuleFileNameFailed";
        initialized_ = false;
        return false;
    }

    executableDirectory_ = modulePath;
    const size_t slash = executableDirectory_.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        lastError_ = L"ExecutableDirectoryUnavailable";
        initialized_ = false;
        return false;
    }

    executableDirectory_.resize(slash);
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
    const std::wstring audioPath = AssetPath(profile);
    const DWORD attributes = GetFileAttributesW(audioPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        SetLastError(L"AudioAssetMissing");
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

std::wstring FireworkAudioPlayer::AssetPath(FireworkAudioProfile profile) const
{
    const wchar_t* fileName = profile == FireworkAudioProfile::Crackle002 ?
        kCrackleAudioFile :
        kImpactAudioFile;
    return executableDirectory_ + L"\\assets\\audio\\" + fileName;
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
