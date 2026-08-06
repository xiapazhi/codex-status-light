/**
 * 文件作用：StatusLight Web Companion 的 MV3 Service Worker
 * 职责范围：
 * 1. 维护到 com.statuslight.web 的 Native Messaging 连接
 * 2. 为 Content Script 消息补充 tabId、windowId、browserInstanceId
 * 3. 处理有限退避重连和快照恢复
 *
 * 不负责：
 * - 读取 ChatGPT 页面 DOM
 * - 决定托盘红黄绿状态
 *
 * 维护说明：
 * - Service Worker 全局变量可能丢失，状态恢复必须依赖 storage 和页面快照
 */
const HOST_NAME = 'com.statuslight.web'
const CHATGPT_URL_PATTERN = 'https://chatgpt.com/*'
const RECONNECT_DELAYS_MS = [1000, 5000, 15000, 30000, 60000]
const MAX_FAST_RECONNECT_ATTEMPTS = 10
const CONNECT_ATTEMPT_TIMEOUT_MS = 5000
const HEARTBEAT_INTERVAL_MS = 1000
const SLOW_RECONNECT_DELAY_MS = 60000
const DIAGNOSTIC_STORAGE_KEY = 'statuslightDiagnostic'
const ICON_ANIMATION_INTERVAL_MS = 125
const ICON_RUNNING_BREATH_PERIOD_MS = 2400
const ICON_FAST_FLASH_INTERVAL_MS = 125
const ICON_WAITING_FLASH_DURATION_MS = 3000
const ICON_COMPLETED_FLASH_DURATION_MS = 1000
const ICON_COMPLETED_VISIBLE_MS = 3000
const ICON_TAB_STATE_TTL_MS = 30000
const WEB_IDLE_STABLE_MS = 2000
const CONTENT_SCRIPT_FILES = [
  'content/conversation-identity.js',
  'content/state-detector.js',
  'content/lifecycle.js',
  'content/chatgpt-observer.js',
]

let nativePort = null
let browserInstanceIdPromise = null
let reconnectAttempt = 0
let reconnectTimer = null
let connectAttemptTimer = null
let lastDesktopConnected = false
let connectionAbnormal = false
let connectionGeneration = 0
let lastIconVisual = 'idle'
let lastIconVisualSince = Date.now()
const conversationStatusByKey = new Map()
const tabConversationKeyById = new Map()
let diagnosticState = {
  nativeConnected: false,
  desktopConnected: false,
  connectionAbnormal: false,
  reconnectAttempt: 0,
  nextReconnectAt: 0,
  lastEvent: 'service_worker_started',
  lastMessageType: '',
  lastError: '',
  updatedAt: Date.now(),
}

function saveDiagnostic(update) {
  diagnosticState = {
    ...diagnosticState,
    ...update,
    nativeConnected: nativePort !== null,
    desktopConnected: lastDesktopConnected,
    connectionAbnormal,
    reconnectAttempt,
    updatedAt: Date.now(),
  }

  chrome.storage.local.set({
    [DIAGNOSTIC_STORAGE_KEY]: diagnosticState,
  }).catch(() => {})
}

function errorText(error) {
  if (!error) {
    return 'unknown error'
  }
  if (typeof error.message === 'string' && error.message) {
    return error.message
  }
  return String(error)
}

function makeMessage(type, payload = {}) {
  return {
    protocolVersion: 1,
    messageId: crypto.randomUUID(),
    type,
    sentAt: Date.now(),
    ...payload,
  }
}

function blendColor(lowColor, highColor, level) {
  const safeLevel = Math.max(0, Math.min(10, level))
  const red = lowColor[0] + Math.round((highColor[0] - lowColor[0]) * safeLevel / 10)
  const green = lowColor[1] + Math.round((highColor[1] - lowColor[1]) * safeLevel / 10)
  const blue = lowColor[2] + Math.round((highColor[2] - lowColor[2]) * safeLevel / 10)
  return `rgb(${red}, ${green}, ${blue})`
}

function iconCenterColor(visual, now) {
  const visualAgeMs = now - lastIconVisualSince
  if (visual === 'running') {
    const phaseMs = now % ICON_RUNNING_BREATH_PERIOD_MS
    const halfPeriodMs = ICON_RUNNING_BREATH_PERIOD_MS / 2
    const risingMs = phaseMs <= halfPeriodMs ? phaseMs : ICON_RUNNING_BREATH_PERIOD_MS - phaseMs
    const level = Math.round(risingMs * 10 / halfPeriodMs)
    return blendColor([205, 122, 0], [255, 196, 0], level)
  }

  if (visual === 'waiting') {
    const shouldBlink = visualAgeMs < ICON_WAITING_FLASH_DURATION_MS
    const blinkOn = Math.floor(now / ICON_FAST_FLASH_INTERVAL_MS) % 2 === 0
    return shouldBlink && blinkOn ? 'rgb(255, 49, 49)' : 'rgb(150, 19, 27)'
  }

  if (visual === 'completed') {
    const shouldBlink = visualAgeMs < ICON_COMPLETED_FLASH_DURATION_MS
    const blinkOn = Math.floor(now / ICON_FAST_FLASH_INTERVAL_MS) % 2 === 0
    return shouldBlink && blinkOn ? 'rgb(34, 220, 116)' : 'rgb(21, 125, 72)'
  }

  return 'rgb(21, 125, 72)'
}

function drawActionIcon(visual) {
  if (typeof OffscreenCanvas === 'undefined') {
    return
  }

  const now = Date.now()
  const centerColor = iconCenterColor(visual, now)
  const canvas = new OffscreenCanvas(32, 32)
  const context = canvas.getContext('2d')
  if (!context) {
    return
  }

  context.clearRect(0, 0, 32, 32)
  context.beginPath()
  context.arc(16, 16, 13, 0, Math.PI * 2)
  context.fillStyle = 'rgba(22, 24, 29, 0.95)'
  context.fill()

  context.beginPath()
  context.arc(16, 16, 10, 0, Math.PI * 2)
  context.fillStyle = centerColor
  context.fill()

  context.beginPath()
  context.arc(16, 16, 14, 0, Math.PI * 2)
  context.strokeStyle = centerColor
  context.lineWidth = 2
  context.stroke()

  try {
    chrome.action.setIcon({
      imageData: context.getImageData(0, 0, 32, 32),
    }, () => {
      void chrome.runtime.lastError
    })
  } catch (error) {
    saveDiagnostic({
      lastEvent: 'set_action_icon_failed',
      lastError: errorText(error),
    })
  }
}

function tabVisualFromState(state) {
  if (state === 'waiting') {
    return 'waiting'
  }
  if (state === 'running') {
    return 'running'
  }
  if (state === 'terminal_success' || state === 'terminal_failed' || state === 'terminal_cancelled') {
    return 'completed'
  }
  return 'idle'
}

function isActiveVisual(visual) {
  return visual === 'running' || visual === 'waiting'
}

function isTerminalState(state) {
  return state === 'terminal_success' ||
    state === 'terminal_failed' ||
    state === 'terminal_cancelled'
}

function buildConversationKey(browserInstanceId, message, tabId) {
  const conversation = message.conversation || {}
  if (conversation.kind === 'persistent' && conversation.id) {
    return `${browserInstanceId}:conversation:${conversation.id}`
  }

  const documentId = message.documentId || 'unknown'
  return `${browserInstanceId}:temporary:${tabId}:${documentId}`
}

function setConversationVisual(conversation, visual, observedAt) {
  if (conversation.visual !== visual) {
    conversation.visual = visual
    conversation.stateChangedAt = observedAt
  }
  conversation.updatedAt = observedAt
}

function completeConversation(conversation, observedAt, now) {
  conversation.operationActive = false
  setConversationVisual(conversation, 'completed', observedAt)
  conversation.completedUntil = now + ICON_COMPLETED_VISIBLE_MS
}

function removeTabConversation(tabId) {
  const conversationKey = tabConversationKeyById.get(tabId)
  tabConversationKeyById.delete(tabId)
  if (!conversationKey) {
    return
  }

  const conversation = conversationStatusByKey.get(conversationKey)
  if (!conversation) {
    return
  }

  conversation.tabIds.delete(tabId)
  if (conversation.tabIds.size === 0) {
    conversationStatusByKey.delete(conversationKey)
  }
}

function currentActionVisual() {
  const now = Date.now()
  let hasRunning = false
  let hasCompleted = false
  for (const [conversationKey, record] of conversationStatusByKey.entries()) {
    if (now - record.updatedAt > ICON_TAB_STATE_TTL_MS) {
      conversationStatusByKey.delete(conversationKey)
      continue
    }

    if (record.completedUntil > 0 && now <= record.completedUntil) {
      hasCompleted = true
      continue
    }

    if (record.visual === 'waiting') {
      return 'waiting'
    }
    if (record.visual === 'running') {
      hasRunning = true
    }
  }

  if (hasCompleted) {
    return 'completed'
  }
  if (hasRunning) {
    return 'running'
  }
  return 'idle'
}

function updateActionIcon() {
  const visual = currentActionVisual()
  const now = Date.now()
  if (visual !== lastIconVisual) {
    lastIconVisual = visual
    lastIconVisualSince = now
  }
  drawActionIcon(visual)
}

function rememberTabState(message, tabId) {
  if (message.type !== 'tab_state') {
    return
  }

  const now = Date.now()
  const observedAt = typeof message.observedAt === 'number' ? message.observedAt : now
  const browserInstanceId = message.browserInstanceId || 'unknown'
  const conversationKey = buildConversationKey(browserInstanceId, message, tabId)
  const visual = tabVisualFromState(message.state)
  let conversation = conversationStatusByKey.get(conversationKey)
  if (!conversation) {
    conversation = {
      visual: 'idle',
      stateChangedAt: observedAt,
      updatedAt: observedAt,
      operationActive: false,
      operationGeneration: 0,
      handledTerminalGeneration: 0,
      completedUntil: 0,
      tabIds: new Set(),
    }
    conversationStatusByKey.set(conversationKey, conversation)
  }

  conversation.tabIds.add(tabId)
  tabConversationKeyById.set(tabId, conversationKey)

  const hasVisibleCompletion = conversation.visual === 'completed' && conversation.completedUntil > now
  if ((visual === 'idle' || visual === 'completed') && hasVisibleCompletion) {
    conversation.updatedAt = observedAt
    updateActionIcon()
    return
  }

  if (isActiveVisual(visual)) {
    if (!conversation.operationActive) {
      conversation.operationGeneration += 1
      conversation.operationActive = true
    }
    conversation.completedUntil = 0
    setConversationVisual(conversation, visual, observedAt)
    updateActionIcon()
    return
  }

  if (isTerminalState(message.state)) {
    if (!conversation.operationActive) {
      updateActionIcon()
      return
    }
    if (conversation.handledTerminalGeneration === conversation.operationGeneration) {
      updateActionIcon()
      return
    }

    conversation.handledTerminalGeneration = conversation.operationGeneration
    completeConversation(conversation, observedAt, now)
    updateActionIcon()
    return
  }

  if (visual === 'idle' && isActiveVisual(conversation.visual)) {
    const stableEnough = observedAt - conversation.stateChangedAt >= WEB_IDLE_STABLE_MS
    if (stableEnough && conversation.handledTerminalGeneration !== conversation.operationGeneration) {
      conversation.handledTerminalGeneration = conversation.operationGeneration
      completeConversation(conversation, observedAt, now)
    } else {
      conversation.updatedAt = observedAt
    }
    updateActionIcon()
    return
  }

  setConversationVisual(conversation, 'idle', observedAt)
  updateActionIcon()
}

async function getBrowserInstanceId() {
  if (browserInstanceIdPromise) {
    return browserInstanceIdPromise
  }

  browserInstanceIdPromise = chrome.storage.local.get('browserInstanceId').then(async (items) => {
    if (items.browserInstanceId) {
      return items.browserInstanceId
    }

    const browserInstanceId = crypto.randomUUID()
    await chrome.storage.local.set({ browserInstanceId })
    return browserInstanceId
  })

  return browserInstanceIdPromise
}

function hasNativeConnection() {
  return nativePort !== null
}

function clearConnectAttemptTimer() {
  if (!connectAttemptTimer) {
    return
  }

  clearTimeout(connectAttemptTimer)
  connectAttemptTimer = null
}

function scheduleReconnect() {
  if (reconnectTimer) {
    return
  }

  const delayIndex = Math.min(reconnectAttempt, RECONNECT_DELAYS_MS.length - 1)
  const hasReachedFastRetryLimit = reconnectAttempt >= MAX_FAST_RECONNECT_ATTEMPTS
  const delayMs = hasReachedFastRetryLimit ? SLOW_RECONNECT_DELAY_MS : RECONNECT_DELAYS_MS[delayIndex]
  const nextReconnectAt = Date.now() + delayMs
  if (!hasReachedFastRetryLimit) {
    reconnectAttempt += 1
  } else {
    connectionAbnormal = true
    updateBadge()
  }
  saveDiagnostic({
    lastEvent: hasReachedFastRetryLimit ? 'auto_connect_failed' : 'schedule_reconnect',
    lastError: '',
    nextReconnectAt,
  })

  reconnectTimer = setTimeout(async () => {
    reconnectTimer = null
    saveDiagnostic({
      lastEvent: 'reconnect_timer_fire',
    })
    connectNative()
  }, delayMs)
}

function connectNative() {
  if (nativePort) {
    return nativePort
  }

  connectionGeneration += 1
  const activeGeneration = connectionGeneration
  try {
    nativePort = chrome.runtime.connectNative(HOST_NAME)
  } catch (error) {
    lastDesktopConnected = false
    saveDiagnostic({
      lastEvent: 'connect_native_throw',
      lastError: errorText(error),
    })
    scheduleReconnect()
    return null
  }

  const connectedPort = nativePort
  const attemptTimeoutAt = Date.now() + CONNECT_ATTEMPT_TIMEOUT_MS
  saveDiagnostic({
    lastEvent: 'native_port_created',
    lastError: '',
    nextReconnectAt: attemptTimeoutAt,
  })

  clearConnectAttemptTimer()
  connectAttemptTimer = setTimeout(() => {
    if (activeGeneration !== connectionGeneration || connectedPort !== nativePort || lastDesktopConnected) {
      return
    }

    connectionGeneration += 1
    nativePort = null
    lastDesktopConnected = false
    saveDiagnostic({
      lastEvent: 'connect_attempt_timeout',
      lastError: '',
      nextReconnectAt: 0,
    })
    try {
      connectedPort.disconnect()
    } catch (error) {
      saveDiagnostic({
        lastEvent: 'connect_attempt_disconnect_failed',
        lastError: errorText(error),
      })
    }
    scheduleReconnect()
  }, CONNECT_ATTEMPT_TIMEOUT_MS)

  connectedPort.onMessage.addListener((message) => {
    if (activeGeneration !== connectionGeneration || connectedPort !== nativePort) {
      return
    }

    if (message.type === 'bridge_status') {
      lastDesktopConnected = Boolean(message.desktopConnected)
      if (lastDesktopConnected) {
        reconnectAttempt = 0
        connectionAbnormal = false
        clearConnectAttemptTimer()
      }
      updateBadge()
      saveDiagnostic({
        lastEvent: 'bridge_status',
        lastMessageType: message.type,
        lastError: '',
        nextReconnectAt: lastDesktopConnected ? 0 : diagnosticState.nextReconnectAt,
      })
      if (lastDesktopConnected) {
        requestAllSnapshots()
      }
    }

    if (message.command && message.command.type === 'focus_tab') {
      executeFocusCommand(message.command)
    }

    if (message.command && message.command.type === 'request_snapshot') {
      executeSnapshotCommand(message.command)
    }
  })

  connectedPort.onDisconnect.addListener(() => {
    if (activeGeneration !== connectionGeneration || connectedPort !== nativePort) {
      return
    }

    clearConnectAttemptTimer()
    const lastError = chrome.runtime.lastError ? chrome.runtime.lastError.message : ''
    nativePort = null
    lastDesktopConnected = false
    updateBadge()
    saveDiagnostic({
      lastEvent: 'native_port_disconnect',
      lastError,
      nextReconnectAt: diagnosticState.nextReconnectAt,
    })
    scheduleReconnect()
  })

  connectedPort.postMessage(makeMessage('extension_hello'))
  saveDiagnostic({
    lastEvent: 'extension_hello_sent',
    lastMessageType: 'extension_hello',
  })
  return nativePort
}

async function sendHeartbeat() {
  const browserInstanceId = await getBrowserInstanceId()
  postToNative(makeMessage('extension_heartbeat', {
    browserInstanceId,
  }))
}

async function executeFocusCommand(command) {
  try {
    if (typeof command.windowId === 'number' && command.windowId > 0) {
      await chrome.windows.update(command.windowId, { focused: true })
    }
    await chrome.tabs.update(command.tabId, { active: true })
    postToNative(makeMessage('focus_tab_result', {
      requestId: command.requestId,
      ok: true,
    }))
  } catch (error) {
    postToNative(makeMessage('focus_tab_result', {
      requestId: command.requestId || '',
      ok: false,
      error: errorText(error),
    }))
  }
}

async function executeSnapshotCommand(command) {
  const requestId = command.requestId || ''
  let checkedTabs = 0
  let updatedTabs = 0
  let failedTabs = 0

  try {
    const tabs = await chrome.tabs.query({ url: CHATGPT_URL_PATTERN })
    checkedTabs = tabs.length

    for (const tab of tabs) {
      if (typeof tab.id !== 'number') {
        failedTabs += 1
        continue
      }

      const isObserverReady = await ensureContentObserver(tab.id)
      if (!isObserverReady) {
        failedTabs += 1
        continue
      }

      try {
        await chrome.tabs.sendMessage(tab.id, makeMessage('request_snapshot'))
        updatedTabs += 1
      } catch (error) {
        failedTabs += 1
        saveDiagnostic({
          lastEvent: 'active_snapshot_failed',
          lastMessageType: 'request_snapshot',
          lastError: errorText(error),
        })
      }
    }

    postToNative(makeMessage('request_snapshot_result', {
      requestId,
      ok: failedTabs === 0,
      checkedTabs,
      updatedTabs,
      failedTabs,
    }))
    saveDiagnostic({
      lastEvent: 'active_snapshot_completed',
      lastMessageType: 'request_snapshot',
      lastError: failedTabs === 0 ? '' : `failed tabs: ${failedTabs}`,
    })
  } catch (error) {
    postToNative(makeMessage('request_snapshot_result', {
      requestId,
      ok: false,
      checkedTabs,
      updatedTabs,
      failedTabs: failedTabs + 1,
      error: errorText(error),
    }))
    saveDiagnostic({
      lastEvent: 'active_snapshot_throw',
      lastMessageType: 'request_snapshot',
      lastError: errorText(error),
    })
  }
}

function postToNative(message) {
  const port = connectNative()
  if (!port) {
    return
  }

  try {
    port.postMessage(message)
    if (lastDesktopConnected) {
      connectionAbnormal = false
    }
    saveDiagnostic({
      lastEvent: 'post_to_native',
      lastMessageType: message.type || 'unknown',
      lastError: '',
    })
  } catch (error) {
    nativePort = null
    lastDesktopConnected = false
    updateBadge()
    saveDiagnostic({
      lastEvent: 'post_to_native_throw',
      lastMessageType: message.type || 'unknown',
      lastError: errorText(error),
    })
    scheduleReconnect()
  }
}

async function ensureContentObserver(tabId) {
  try {
    await chrome.tabs.sendMessage(tabId, makeMessage('statuslight_probe'))
    saveDiagnostic({
      lastEvent: 'content_probe_ok',
      lastMessageType: 'statuslight_probe',
      lastError: '',
    })
    return true
  } catch (probeError) {
    saveDiagnostic({
      lastEvent: 'content_probe_failed',
      lastMessageType: 'statuslight_probe',
      lastError: errorText(probeError),
    })
  }

  try {
    await chrome.scripting.executeScript({
      target: {
        tabId,
        allFrames: false,
      },
      files: CONTENT_SCRIPT_FILES,
    })
    saveDiagnostic({
      lastEvent: 'content_injected',
      lastMessageType: 'executeScript',
      lastError: '',
    })
    return true
  } catch (injectError) {
    saveDiagnostic({
      lastEvent: 'content_inject_failed',
      lastMessageType: 'executeScript',
      lastError: errorText(injectError),
    })
    return false
  }
}

async function forwardContentMessage(message, sender) {
  const tab = sender.tab
  if (!tab || typeof tab.id !== 'number') {
    return
  }

  const browserInstanceId = await getBrowserInstanceId()
  saveDiagnostic({
    lastEvent: 'content_message_received',
    lastMessageType: message.type || 'unknown',
    lastObservedState: message.state || '',
    lastObservedReason: message.reason || '',
  })
  rememberTabState({
    ...message,
    browserInstanceId,
  }, tab.id)
  postToNative({
    ...message,
    browserInstanceId,
    tabId: tab.id,
    windowId: tab.windowId || 0,
  })
}

async function requestAllSnapshots() {
  saveDiagnostic({
    lastEvent: 'snapshot_requested',
    lastMessageType: 'snapshot_begin',
  })
  postToNative(makeMessage('snapshot_begin'))
  const tabs = await chrome.tabs.query({ url: CHATGPT_URL_PATTERN })
  for (const tab of tabs) {
    if (typeof tab.id === 'number') {
      const isObserverReady = await ensureContentObserver(tab.id)
      if (isObserverReady) {
        chrome.tabs.sendMessage(tab.id, makeMessage('request_snapshot')).catch((error) => {
          saveDiagnostic({
            lastEvent: 'request_snapshot_failed',
            lastMessageType: 'request_snapshot',
            lastError: errorText(error),
          })
        })
      }
    }
  }
  postToNative(makeMessage('snapshot_end'))
}

function updateBadge() {
  if (lastDesktopConnected) {
    chrome.action.setBadgeText({ text: '' })
    return
  }

  if (connectionAbnormal) {
    chrome.action.setBadgeText({ text: '×' })
    chrome.action.setBadgeBackgroundColor({ color: '#d62828' })
    return
  }

  chrome.action.setBadgeText({ text: '!' })
  chrome.action.setBadgeBackgroundColor({ color: '#ffb000' })
}

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  if (message && message.channel === 'statuslight_content') {
    forwardContentMessage(message.payload, sender)
    sendResponse({ ok: true })
    return true
  }

  if (message && message.type === 'popup_status') {
    Promise.all([
      chrome.tabs.query({ url: CHATGPT_URL_PATTERN }),
      chrome.storage.local.get(DIAGNOSTIC_STORAGE_KEY),
    ]).then(([tabs, items]) => {
      const diagnostic = items[DIAGNOSTIC_STORAGE_KEY] || diagnosticState
      sendResponse({
        desktopConnected: Boolean(diagnostic.desktopConnected || lastDesktopConnected),
        chatgptTabCount: tabs.length,
        nativeConnected: Boolean(diagnostic.nativeConnected || hasNativeConnection()),
        connectionAbnormal: Boolean(diagnostic.connectionAbnormal || connectionAbnormal),
        reconnectAttempt: diagnostic.reconnectAttempt || reconnectAttempt,
        maxFastReconnectAttempts: MAX_FAST_RECONNECT_ATTEMPTS,
        nextReconnectAt: diagnostic.nextReconnectAt || diagnosticState.nextReconnectAt || 0,
        diagnostic,
      })
    })
    return true
  }

  if (message && message.type === 'popup_reconnect') {
    reconnectAttempt = 0
    connectionAbnormal = false
    if (reconnectTimer) {
      clearTimeout(reconnectTimer)
      reconnectTimer = null
    }
    clearConnectAttemptTimer()
    saveDiagnostic({
      lastEvent: 'popup_reconnect_clicked',
      lastError: '',
      nextReconnectAt: 0,
    })
    if (nativePort) {
      connectionGeneration += 1
      nativePort.disconnect()
      nativePort = null
    }
    connectNative()
    sendResponse({ ok: true })
    return true
  }

  if (message && message.type === 'popup_snapshot') {
    requestAllSnapshots()
    sendResponse({ ok: true })
    return true
  }

  return false
})

chrome.tabs.onRemoved.addListener(async (tabId) => {
  removeTabConversation(tabId)
  updateActionIcon()
  const browserInstanceId = await getBrowserInstanceId()
  postToNative(makeMessage('tab_removed', { browserInstanceId, tabId }))
})

chrome.tabs.onUpdated.addListener(async (tabId, changeInfo, tab) => {
  const isChatGptTab = typeof tab.url === 'string' && tab.url.startsWith('https://chatgpt.com/')
  if (isChatGptTab && changeInfo.status === 'complete') {
    ensureContentObserver(tabId)
  }

  if (changeInfo.discarded || changeInfo.frozen) {
    removeTabConversation(tabId)
    updateActionIcon()
    const browserInstanceId = await getBrowserInstanceId()
    postToNative(makeMessage('tab_suspended', {
      browserInstanceId,
      tabId,
      documentId: 'unknown',
      reason: changeInfo.discarded ? 'discarded' : 'frozen',
    }))
  }
})

chrome.runtime.onStartup.addListener(() => {
  saveDiagnostic({
    lastEvent: 'runtime_startup',
    lastError: '',
  })
  connectNative()
})

chrome.runtime.onInstalled.addListener(() => {
  reconnectAttempt = 0
  connectionAbnormal = false
  saveDiagnostic({
    lastEvent: 'runtime_installed',
    lastError: '',
  })
  connectNative()
})

updateBadge()
updateActionIcon()
saveDiagnostic({
  lastEvent: 'service_worker_ready',
})
connectNative()
setInterval(sendHeartbeat, HEARTBEAT_INTERVAL_MS)
setInterval(updateActionIcon, ICON_ANIMATION_INTERVAL_MS)
