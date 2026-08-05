/**
 * 文件作用：声明 P1 控制台状态命令
 * 职责范围：
 * 1. 调用 StatusReader 读取状态快照
 * 2. 将会话状态和额度摘要打印到控制台
 * 3. 支持 watch 模式定期刷新
 *
 * 不负责：
 * - JSONL 事件解析细节
 * - 托盘 UI 和图标渲染
 *
 * 维护说明：
 * - 状态判断统一维护在 StatusReader 中，本文件只保留命令行展示逻辑
 */
#pragma once

#include "StatusReader.h"

struct StatusOptions {
    std::wstring codexHome;
    size_t maxFiles = 30;
    int recentHours = 24;
    int pollSeconds = 2;
    bool watch = false;
};

class StatusCommand {
public:
    int Run(const StatusOptions& options);

private:
    void PrintStatus(const StatusSnapshot& snapshot) const;
    std::string TaskStateToString(TaskState state) const;
    std::string QuotaValidityToString(QuotaValidity validity) const;
};
