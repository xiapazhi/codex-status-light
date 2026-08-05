/**
 * 文件作用：StatusLight Web Companion Popup 交互
 * 职责范围：
 * 1. 展示桌面桥接连接状态
 * 2. 展示自动互联重试次数和下次连接倒计时
 * 3. 提供未连接时的立即连接按钮
 *
 * 不负责：
 * - 显示任务红黄绿状态
 * - 读取 ChatGPT 页面内容
 *
 * 维护说明：
 * - Popup 只做连接控制，不展示底层协议诊断明细
 */
let lastStatusResponse = null
let countdownTimer = null

function refreshStatus() {
  chrome.runtime.sendMessage({ type: 'popup_status' }, (response) => {
    if (!response) {
      return
    }

    lastStatusResponse = response
    setStatus('desktopStatus', response.desktopConnected)
    setConnectStatus(response)
    updateCountdown()
  })
}

function setStatus(elementId, isConnected) {
  const element = document.getElementById(elementId)
  element.textContent = isConnected ? '已连接' : '未连接'
  element.classList.toggle('status-on', isConnected)
  element.classList.toggle('status-off', !isConnected)
}

function setConnectStatus(response) {
  const element = document.getElementById('connectStatus')
  const reconnectButton = document.getElementById('reconnectButton')

  if (response.desktopConnected) {
    element.textContent = '正常'
    element.classList.toggle('status-on', true)
    element.classList.toggle('status-warn', false)
    element.classList.toggle('status-off', false)
    reconnectButton.hidden = true
    return
  }

  reconnectButton.hidden = false
  if (response.connectionAbnormal) {
    element.textContent = '异常'
    element.classList.toggle('status-on', false)
    element.classList.toggle('status-warn', false)
    element.classList.toggle('status-off', true)
    return
  }

  element.textContent = '重试中'
  element.classList.toggle('status-on', false)
  element.classList.toggle('status-warn', true)
  element.classList.toggle('status-off', false)
}

function updateCountdown() {
  const nextActionRow = document.getElementById('nextActionRow')
  const nextReconnect = document.getElementById('nextReconnect')
  if (!lastStatusResponse || lastStatusResponse.desktopConnected) {
    nextActionRow.hidden = true
    nextReconnect.textContent = '-'
    return
  }

  nextActionRow.hidden = false
  const nextReconnectAt = lastStatusResponse.nextReconnectAt || 0
  if (!nextReconnectAt) {
    nextReconnect.textContent = '马上重试'
    return
  }

  const remainingMs = Math.max(0, nextReconnectAt - Date.now())
  nextReconnect.textContent = `${Math.ceil(remainingMs / 1000)}s`
}

function startCountdown() {
  if (countdownTimer) {
    return
  }

  countdownTimer = setInterval(() => {
    updateCountdown()
    const shouldRefresh = lastStatusResponse &&
      !lastStatusResponse.desktopConnected &&
      lastStatusResponse.nextReconnectAt > 0 &&
      Date.now() >= lastStatusResponse.nextReconnectAt
    if (shouldRefresh) {
      refreshStatus()
    }
  }, 1000)
}

function stopCountdown() {
  if (!countdownTimer) {
    return
  }

  clearInterval(countdownTimer)
  countdownTimer = null
}

document.getElementById('reconnectButton').addEventListener('click', () => {
  const reconnectButton = document.getElementById('reconnectButton')
  reconnectButton.disabled = true
  reconnectButton.textContent = '正在连接...'
  chrome.runtime.sendMessage({ type: 'popup_reconnect' }, refreshStatus)
  setTimeout(() => {
    reconnectButton.disabled = false
    reconnectButton.textContent = '立即连接'
    refreshStatus()
  }, 1200)
})

window.addEventListener('unload', stopCountdown)
refreshStatus()
startCountdown()
