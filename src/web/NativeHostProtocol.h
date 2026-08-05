/**
 * 文件作用：声明 Chrome Native Messaging 与本地 Named Pipe 共用协议工具
 * 职责范围：
 * 1. 读写 4 字节长度 + UTF-8 JSON 的消息帧
 * 2. 生成当前用户隔离的 WebBridge Named Pipe 名称
 * 3. 集中维护 Native Host 名称和允许的扩展来源
 *
 * 不负责：
 * - 解释 ChatGPT 页面状态
 * - 修改托盘聚合状态
 *
 * 维护说明：
 * - stdout 只能写 Native Messaging 协议数据，调试信息必须写 stderr
 */
#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

namespace NativeHostProtocol {

constexpr uint32_t kProtocolVersion = 1;
constexpr uint32_t kMaxMessageBytes = 64 * 1024;
constexpr const wchar_t* kNativeHostName = L"com.statuslight.web";
constexpr const wchar_t* kCompanionExtensionId = L"pkaefmgibeeemjoilbpopeiffmkbnjoi";
constexpr const wchar_t* kAllowedExtensionOrigin = L"chrome-extension://pkaefmgibeeemjoilbpopeiffmkbnjoi/";

std::wstring WebBridgePipeName();
std::wstring Utf8ToWide(const std::string& value);
std::string WideToUtf8(const std::wstring& value);
std::string JsonEscape(const std::string& value);
std::string MakeMessage(const std::string& type);
std::string MakeBridgeStatus(bool desktopConnected);
std::string MakeProtocolError(const std::string& reason);
bool IsAllowedExtensionOrigin(const std::wstring& value);

bool ReadFramedMessage(HANDLE input, std::string* message, std::wstring* errorMessage);
bool WriteFramedMessage(HANDLE output, const std::string& message, std::wstring* errorMessage);

} // namespace NativeHostProtocol
