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
    lastError_ = L"None";
    return true;
}

void FireworkAudioPlayer::PlayExplosion(FireworkAudioProfile profile)
{
    if (!initialized_ && !Initialize()) {
        return;
    }

    const std::wstring audioPath = AssetPath(profile);
    const DWORD attributes = GetFileAttributesW(audioPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        lastError_ = L"AudioAssetMissing";
        return;
    }

    SendMciCommand(L"close " + std::wstring(kAudioAlias));
    const std::wstring openCommand =
        L"open \"" + audioPath + L"\" type mpegvideo alias " + std::wstring(kAudioAlias);
    if (!SendMciCommand(openCommand)) {
        return;
    }

    SendMciCommand(L"play " + std::wstring(kAudioAlias));
}

void FireworkAudioPlayer::Shutdown()
{
    SendMciCommand(L"close " + std::wstring(kAudioAlias));
}

std::wstring FireworkAudioPlayer::LastError() const
{
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
        lastError_ = L"None";
        return true;
    }

    lastError_ = MciErrorText(error);
    return false;
}
