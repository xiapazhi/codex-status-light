/**
 * 文件作用：实现 Codex 进程检测器
 * 职责范围：
 * 1. 通过 CreateToolhelp32Snapshot 枚举 Windows 进程
 * 2. 根据集中维护的可执行文件名识别 Codex Desktop / CLI
 * 3. 返回进程数量给托盘生命周期和诊断信息
 *
 * 不负责：
 * - 读取窗口标题
 * - 推断任务状态颜色
 *
 * 维护说明：
 * - 只读进程列表，不结束、不注入、不修改任何外部进程
 */
#include "ProcessMonitor.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cwctype>

namespace {

const wchar_t* const kCodexProcessNames[] = {
    L"codex.exe",
    L"codex-cli.exe",
    L"codex-x86_64-pc-windows-msvc.exe",
    L"codex-win32-x64.exe",
    L"codex desktop.exe"
};

} // namespace

ProcessSnapshot ProcessMonitor::ReadOnce() const
{
    ProcessSnapshot snapshot;
    HANDLE processSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processSnapshot == INVALID_HANDLE_VALUE) {
        snapshot.errorMessage = L"CreateToolhelp32Snapshot failed";
        return snapshot;
    }

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);

    if (!Process32FirstW(processSnapshot, &entry)) {
        snapshot.errorMessage = L"Process32FirstW failed";
        CloseHandle(processSnapshot);
        return snapshot;
    }

    do {
        const std::wstring processName = entry.szExeFile;
        if (!IsCodexProcessName(processName)) {
            continue;
        }

        snapshot.codexProcessCount++;
        snapshot.processNames.push_back(processName);
    } while (Process32NextW(processSnapshot, &entry));

    CloseHandle(processSnapshot);
    return snapshot;
}

bool ProcessMonitor::IsCodexProcessName(const std::wstring& processName) const
{
    const std::wstring lowerName = ToLower(processName);
    for (const wchar_t* targetName : kCodexProcessNames) {
        if (lowerName == targetName) {
            return true;
        }
    }
    return false;
}

std::wstring ProcessMonitor::ToLower(const std::wstring& value) const
{
    std::wstring result = value;
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(towlower(character));
    });
    return result;
}
