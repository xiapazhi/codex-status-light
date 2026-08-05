/**
 * 文件作用：提取 ChatGPT 当前页面的对话标识
 * 职责范围：
 * 1. 从 /c/<conversation-id> URL 中提取正式对话 ID
 * 2. 为尚未生成 ID 的新对话返回 temporary 标识
 *
 * 不负责：
 * - 读取页面标题或聊天标题
 * - 读取 ChatGPT 账号信息
 *
 * 维护说明：
 * - 输出只允许包含 URL 路径中的 conversationId，不允许传输正文
 */
window.StatusLightConversationIdentity = {
  read() {
    const match = window.location.pathname.match(/^\/c\/([^/?#]+)/)
    if (match && match[1]) {
      return {
        kind: 'persistent',
        id: match[1],
      }
    }

    return {
      kind: 'temporary',
      id: '',
    }
  },
}
