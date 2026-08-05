/**
 * 文件作用：声明 ChatGPT Web 监听诊断文本生成器
 * 职责范围：
 * 1. 输出扩展桥接、Observer、Conversation 和协议错误计数
 * 2. 对 conversationKey 与 owner observer 做截断展示
 *
 * 不负责：
 * - 读取网页正文
 * - 修改监听状态
 *
 * 维护说明：
 * - 诊断信息只展示技术状态和短 ID，不展示账号、标题、聊天内容或完整 URL
 */
#pragma once

#include "WebTypes.h"

#include <string>

class WebDiagnostics {
public:
    std::wstring Build(const WebAccountState& state, bool enabled) const;

private:
    std::wstring Widen(const std::string& value) const;
    std::wstring StateText(WebConversationState state) const;
    std::string ShortId(const std::string& value) const;
};
