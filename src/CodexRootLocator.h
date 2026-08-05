/**
 * 文件作用：定位本机 Codex 数据根目录
 * 职责范围：
 * 1. 读取命令行和环境变量中的 Codex 根目录
 * 2. 校验根目录和 sessions 子目录是否可读
 * 3. 返回 P0 取证工具可直接使用的路径
 *
 * 不负责：
 * - 创建或修改 .codex 内任何文件
 * - 解析 rollout JSONL
 *
 * 维护说明：
 * - 当前实现保持只读校验，后续托盘阶段可复用定位结果
 */
#pragma once

#include <string>

struct CodexRootResult {
    std::wstring rootPath;
    std::wstring sessionsPath;
    bool isUsable = false;
    std::wstring errorMessage;
};

class CodexRootLocator {
public:
    CodexRootResult Locate(const std::wstring& commandLineCodexHome) const;

private:
    std::wstring ReadEnvironmentVariable(const wchar_t* name) const;
    bool IsReadableDirectory(const std::wstring& path) const;
};
