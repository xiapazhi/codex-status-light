/**
 * 文件作用：声明 Codex 进程检测器
 * 职责范围：
 * 1. 枚举本机进程并识别 Codex Desktop / CLI 相关进程
 * 2. 提供进程数量和最近检测错误，供托盘生命周期和诊断使用
 * 3. 将目标进程名称集中维护，避免散落在业务代码里
 *
 * 不负责：
 * - 根据进程存在推断任务正在运行
 * - 启动或结束任何 Codex 进程
 *
 * 维护说明：
 * - 进程检测只能作为辅助证据，任务状态仍必须来自本地结构化 JSONL 事件
 */
#pragma once

#include <string>
#include <vector>

struct ProcessSnapshot {
    size_t codexProcessCount = 0;
    std::vector<std::wstring> processNames;
    std::wstring errorMessage;
};

class ProcessMonitor {
public:
    ProcessSnapshot ReadOnce() const;

private:
    bool IsCodexProcessName(const std::wstring& processName) const;
    std::wstring ToLower(const std::wstring& value) const;
};
