/**
 * 文件作用：声明 Chrome Native Messaging Host 注册器
 * 职责范围：
 * 1. 生成 Native Host manifest 文件
 * 2. 写入 HKCU Chrome NativeMessagingHosts 注册表键
 * 3. 使用当前 StatusLight.exe 路径自动修复移动后的安装位置
 *
 * 不负责：
 * - 安装 Chrome 扩展
 * - 修改 Chrome Profile 或读取浏览器数据
 *
 * 维护说明：
 * - 这里只写 HKCU，不要求管理员权限
 */
#pragma once

#include <string>

class NativeHostRegistration {
public:
    bool EnsureRegistered(std::wstring* errorMessage) const;

private:
    std::wstring ManifestPath() const;
    std::wstring CurrentExecutablePath() const;
    bool EnsureDirectory(const std::wstring& path, std::wstring* errorMessage) const;
    bool WriteManifest(const std::wstring& manifestPath, const std::wstring& exePath, std::wstring* errorMessage) const;
    bool WriteRegistry(const std::wstring& manifestPath, std::wstring* errorMessage) const;
};
