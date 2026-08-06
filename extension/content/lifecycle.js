/**
 * 文件作用：封装 ChatGPT 标签页生命周期状态
 * 职责范围：
 * 1. 记录页面是否进入 freeze/pagehide 等暂停状态
 * 2. 在恢复时请求观察器重新发送快照
 *
 * 不负责：
 * - 判断任务是否完成
 * - 读取页面正文
 *
 * 维护说明：
 * - 标签页隐藏不等于暂停，只有明确 lifecycle 事件才标记 suspended
 */
window.StatusLightLifecycle = {
  suspended: false,
  onResumeCallbacks: [],
  onResume(callback) {
    this.onResumeCallbacks.push(callback)
  },
  notifyResume() {
    this.suspended = false
    for (const callback of this.onResumeCallbacks) {
      callback()
    }
  },
}

document.addEventListener('freeze', () => {
  window.StatusLightLifecycle.suspended = true
})

document.addEventListener('resume', () => {
  window.StatusLightLifecycle.notifyResume()
})

window.addEventListener('pageshow', () => {
  window.StatusLightLifecycle.notifyResume()
})

document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') {
    window.StatusLightLifecycle.notifyResume()
  }
})

window.addEventListener('focus', () => {
  window.StatusLightLifecycle.notifyResume()
})

window.addEventListener('pagehide', () => {
  window.StatusLightLifecycle.suspended = true
})
