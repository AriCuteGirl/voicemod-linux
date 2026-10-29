// Preload for the Action Wheel overlay. Runs in the page's own world (contextIsolation off)
// so it can wrap WebSocket before Voicemod's wheel bundle opens its Brigitte connection.
//
// On Windows the engine opens the wheel from a global keyboard hook and feeds it raw mouse
// deltas. Neither reaches a Wine app on Wayland, so here:
//   - main.js tells us to show/hide (from a Hyprland bind) and we deliver the engine's own
//     WheelShown/WheelHidden events to the page;
//   - the wheel's feature flags are rewritten to useNativeMouse, so it follows ordinary
//     DOM mouse events in this window.
const { ipcRenderer } = require('electron');

const params = new URLSearchParams(location.search);
window.config = {
  getVoicemodConfig: async () => JSON.stringify({
    language: 'en',
    os: 'Windows',
    version: params.get('appVersion') || 'unknown',
    deviceId: params.get('deviceId') || 'unknown',
    environment: 'production',
  }),
  writeDebugLog: async () => {},
};

const NativeWebSocket = window.WebSocket;
let bridge = null;

function rewriteIncoming(data) {
  try {
    const message = JSON.parse(data);
    const object = message?.bridgeData?.data?.object;
    if (object && object.sender === 'wheel' && object.wheel && typeof object.wheel === 'object') {
      object.wheel.useNativeMouse = true;
      return JSON.stringify(message);
    }
  } catch { /* not JSON: pass through */ }
  return data;
}

function wrapHandler(handler) {
  return (event) => {
    const data = rewriteIncoming(event.data);
    if (data === event.data) return handler(event);
    return handler(new MessageEvent('message', { data, origin: event.origin }));
  };
}

window.WebSocket = class extends NativeWebSocket {
  constructor(...args) {
    super(...args);
    bridge = this;
  }

  set onmessage(handler) {
    super.onmessage = handler ? wrapHandler(handler) : handler;
  }

  get onmessage() {
    return super.onmessage;
  }

  addEventListener(type, handler, options) {
    if (type === 'message' && typeof handler === 'function') {
      return super.addEventListener(type, wrapHandler(handler), options);
    }
    return super.addEventListener(type, handler, options);
  }

  send(data) {
    // The page sends WheelHidden once it has finished closing; hide the overlay window then.
    if (String(data).includes('WheelHidden')) ipcRenderer.send('wheel-closed');
    return super.send(data);
  }
};

function deliver(type) {
  if (!bridge || bridge.readyState !== NativeWebSocket.OPEN) return;
  const id = crypto.randomUUID();
  const data = JSON.stringify({ bridgeData: { data: { type, id, object: {} } } });
  bridge.dispatchEvent(new MessageEvent('message', { data }));
}

ipcRenderer.on('wheel-show', () => deliver('WheelShown'));
// Like releasing the key on Windows: runs the highlighted action, then the page closes itself.
ipcRenderer.on('wheel-release', () => deliver('WheelHidden'));
