/**
 * 文件作用：集中声明 StatusLight 的应用版本和更新源。
 * 职责范围：提供命令行、诊断信息和自动更新模块共用的版本常量。
 * 不负责：运行时检查更新、下载文件或替换 EXE。
 * 维护说明：发布前运行 tools\Bump-Version.ps1 递增版本，并使用 v 前缀创建 Gitee Release tag。
 */
#pragma once

namespace AppVersion {

constexpr const char* kStatusLightVersion = "1.2.1";
constexpr const wchar_t* kStatusLightVersionWide = L"1.2.1";
constexpr const char* kReleaseOwner = "yuan_yi";
constexpr const char* kReleaseRepo = "codex-status-light";

} // namespace AppVersion
