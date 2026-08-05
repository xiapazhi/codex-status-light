/**
 * 文件作用：检测 ChatGPT 页面任务状态
 * 职责范围：
 * 1. 识别运行、等待、终止和空闲状态
 * 2. 将 DOM 信号转换为固定枚举和布尔信号
 *
 * 不负责：
 * - 将按钮原始文字发送给本地程序
 * - 自动点击或修改页面
 *
 * 维护说明：
 * - 选择器必须尽量限制在主内容区域，避免侧边栏加载状态误判为任务运行
 */
function findMainRoot() {
  return document.querySelector('main') ||
    document.querySelector('[role="main"]') ||
    document.body
}

function hasElement(root, selector) {
  return Boolean(root && root.querySelector(selector))
}

function hasStopControl(root) {
  return hasElement(root, 'button[data-testid="stop-button"]') ||
    hasElement(root, 'button[aria-label="Stop streaming"]') ||
    hasElement(root, 'button[aria-label="Stop generating"]')
}

function hasProgress(root) {
  return hasElement(root, '[role="progressbar"]')
}

function hasBusyAssistant(root) {
  const busyItems = root.querySelectorAll('[aria-busy="true"]')
  for (const item of busyItems) {
    const inMain = item.closest('main') || item.closest('[role="main"]')
    if (inMain) {
      return true
    }
  }
  return false
}

function hasExplicitGate(root) {
  const gateSelectors = [
    'button[data-testid*="approve" i]',
    'button[data-testid*="confirm" i]',
    'button[data-testid*="continue" i]',
    '[role="radio"]',
    'input[type="radio"]',
    'input[type="checkbox"]',
    'select',
  ]

  return gateSelectors.some((selector) => hasElement(root, selector))
}

function hasTerminalFailure(root) {
  return hasElement(root, 'button[data-testid*="retry" i]') ||
    hasElement(root, 'button[aria-label*="Retry" i]') ||
    hasElement(root, 'button[aria-label*="Try again" i]')
}

window.StatusLightStateDetector = {
  detect() {
    const root = findMainRoot()
    const signals = {
      stopControl: hasStopControl(root),
      busy: hasBusyAssistant(root),
      toolActivity: hasProgress(root),
      explicitGate: hasExplicitGate(root),
      terminalFailure: hasTerminalFailure(root),
      terminalCancelled: false,
    }

    let state = 'idle'
    let reason = 'no_active_signal'
    if (signals.explicitGate && !signals.stopControl) {
      state = 'waiting'
      reason = 'explicit_gate'
    } else if (signals.stopControl || signals.busy || signals.toolActivity) {
      state = 'running'
      reason = signals.stopControl ? 'visible_stop_control' : 'active_task_signal'
    } else if (signals.terminalFailure) {
      state = 'terminal_failed'
      reason = 'retry_available'
    }

    return {
      state,
      reason,
      signals,
    }
  },
}
