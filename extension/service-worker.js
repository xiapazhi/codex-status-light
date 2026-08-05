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
const SLOW_RECONNECT_DELAY_MS = 60000
const DIAGNOSTIC_STORAGE_KEY = 'statuslightDiagnostic'
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
  })
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
  const browserInstanceId = await getBrowserInstanceId()
  postToNative(makeMessage('tab_removed', { browserInstanceId, tabId }))
})

chrome.tabs.onUpdated.addListener(async (tabId, changeInfo, tab) => {
  const isChatGptTab = typeof tab.url === 'string' && tab.url.startsWith('https://chatgpt.com/')
  if (isChatGptTab && changeInfo.status === 'complete') {
    ensureContentObserver(tabId)
  }

  if (changeInfo.discarded || changeInfo.frozen) {
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
saveDiagnostic({
  lastEvent: 'service_worker_ready',
})
connectNative()
