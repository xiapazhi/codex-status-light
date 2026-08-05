/**
 * 文件作用：ChatGPT 页面状态观察器入口
 * 职责范围：
 * 1. 建立单个页面级 MutationObserver
 * 2. 使用带尾部调用的节流扫描 DOM 状态
 * 3. 只在状态变化或快照请求时上报结构化状态
 *
 * 不负责：
 * - 传输聊天正文、标题、按钮文字或浏览器存储
 * - 决定托盘状态颜色
 *
 * 维护说明：
 * - 持续流式输出时不能使用纯尾部防抖，否则短任务可能漏检
 */
const STATUSLIGHT_CHANNEL = 'statuslight_content'
const THROTTLE_MS = 300

if (!window.__StatusLightObserverInstalled) {
  window.__StatusLightObserverInstalled = true

  let documentId = crypto.randomUUID()
  let sequence = 0
  let lastPayloadKey = ''
  let lastScanAt = 0
  let trailingTimer = null
  let lastUrl = window.location.href
  let observer = null
  let isContextInvalidated = false

  function makeMessage(type, payload = {}) {
    return {
      protocolVersion: 1,
      messageId: crypto.randomUUID(),
      type,
      sentAt: Date.now(),
      ...payload,
    }
  }

  function stopObserver() {
    isContextInvalidated = true
    if (trailingTimer) {
      clearTimeout(trailingTimer)
      trailingTimer = null
    }
    if (observer) {
      observer.disconnect()
    }
    window.__StatusLightObserverInstalled = false
  }

  function isInvalidatedError(error) {
    return error && String(error.message || error).includes('Extension context invalidated')
  }

  function postPayload(payload) {
    if (isContextInvalidated) {
      return
    }

    try {
      const result = chrome.runtime.sendMessage({
        channel: STATUSLIGHT_CHANNEL,
        payload,
      })
      if (result && typeof result.catch === 'function') {
        result.catch((error) => {
          if (isInvalidatedError(error)) {
            stopObserver()
          }
        })
      }
    } catch (error) {
      if (isInvalidatedError(error)) {
        stopObserver()
      }
    }
  }

  function buildStatePayload() {
    const detected = window.StatusLightStateDetector.detect()
    sequence += 1
    return makeMessage('tab_state', {
      documentId,
      conversation: window.StatusLightConversationIdentity.read(),
      state: detected.state,
      reason: detected.reason,
      signals: detected.signals,
      visibility: document.visibilityState,
      observedAt: Date.now(),
      sequence,
    })
  }

  function scanNow(force = false) {
    if (isContextInvalidated) {
      return
    }

    if (window.StatusLightLifecycle.suspended) {
      postPayload(makeMessage('tab_suspended', {
        documentId,
        reason: 'page_lifecycle_suspended',
      }))
      return
    }

    const payload = buildStatePayload()
    const payloadKey = JSON.stringify({
      conversation: payload.conversation,
      state: payload.state,
      reason: payload.reason,
      signals: payload.signals,
      visibility: payload.visibility,
    })

    if (!force && payloadKey === lastPayloadKey) {
      return
    }

    lastPayloadKey = payloadKey
    postPayload(payload)
  }

  function scheduleScan() {
    if (isContextInvalidated) {
      return
    }

    const now = Date.now()
    const elapsed = now - lastScanAt
    if (elapsed >= THROTTLE_MS) {
      lastScanAt = now
      scanNow(false)
      return
    }

    if (trailingTimer) {
      return
    }

    trailingTimer = setTimeout(() => {
      trailingTimer = null
      lastScanAt = Date.now()
      scanNow(false)
    }, THROTTLE_MS - elapsed)
  }

  function watchUrlChanges() {
    if (window.location.href === lastUrl) {
      return
    }

    lastUrl = window.location.href
    documentId = crypto.randomUUID()
    sequence = 0
    lastPayloadKey = ''
    scanNow(true)
  }

  function observerRoot() {
    return document.querySelector('main') ||
      document.querySelector('[role="main"]') ||
      document.body
  }

  observer = new MutationObserver(() => {
    if (isContextInvalidated) {
      return
    }

    watchUrlChanges()
    scheduleScan()
  })

  observer.observe(observerRoot(), {
    childList: true,
    subtree: true,
    characterData: true,
    attributes: true,
    attributeFilter: [
      'class',
      'aria-hidden',
      'aria-busy',
      'aria-label',
      'aria-disabled',
      'disabled',
      'data-testid',
      'data-state',
      'role',
    ],
  })

  chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message && message.type === 'statuslight_probe') {
      sendResponse({
        ok: true,
        documentId,
      })
      return false
    }

    if (message && message.type === 'request_snapshot') {
      scanNow(true)
    }

    return false
  })

  window.StatusLightLifecycle.onResume(() => {
    scanNow(true)
  })

  postPayload(makeMessage('tab_registered', {
    documentId,
    conversation: window.StatusLightConversationIdentity.read(),
  }))
  scanNow(true)
}
