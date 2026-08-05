/**
 * 文件作用：声明 Web 监听链路的本地调试日志工具
 * 职责范围：
 * 1. 提供 Web 扩展桥接链路统一日志入口
 * 2. 输出 Native Host、Named Pipe、状态聚合的关键运行节点
 * 3. 提供日志文件路径，方便用户和命令行自检定位问题
 *
 * 不负责：
 * - 记录网页正文、聊天内容或账号敏感信息
 * - 替代主程序托盘 UI 状态展示
 *
 * 维护说明：
 * - stdout 被 Chrome Native Messaging 协议占用，Web 桥接日志必须写入文件
 */
#pragma once

#include <string>

namespace WebDebugLog {

std::wstring LogPath();
void Write(const wchar_t* component, const std::wstring& message);
void WriteUtf8(const wchar_t* component, const std::string& message);

} // namespace WebDebugLog
