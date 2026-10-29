// Stands in for the QWebChannel `config` object that Voicemod's Qt shell injects.
// The frontend only needs getVoicemodConfig() and writeDebugLog(); everything else
// goes over the Brigitte websocket. `launcher` feeds progress to launcher.html.
const { contextBridge, ipcRenderer } = require('electron');

const params = new URLSearchParams(location.search);

contextBridge.exposeInMainWorld('config', {
  getVoicemodConfig: async () => JSON.stringify({
    language: params.get('lang') || 'en',
    os: 'Windows',
    osVersion: 'Windows 10',
    version: params.get('version') || 'unknown',
    deviceId: params.get('deviceId') || 'unknown',
    environment: 'production',
    partnerName: '',
    partnerTrackingId: '',
    experimentsAttributes: {},
  }),
  writeDebugLog: async (text) => ipcRenderer.invoke('debug-log', String(text)),
  openUrlRequest: async (url) => ipcRenderer.invoke('open-url', String(url)),
});

// "Music through mic" button: a small panel of our own on top of Voicemod's page that sends
// other apps' sound (Spotify, a browser...) out through the virtual mic as well.
const MUSIC_CSS = `
#vml-music-btn{position:fixed;right:16px;bottom:16px;z-index:2147483000;width:40px;height:40px;
  border-radius:50%;border:1px solid #2a2a33;background:#17171d;color:#e8e8ef;cursor:pointer;
  display:flex;align-items:center;justify-content:center;box-shadow:0 4px 14px #0008}
#vml-music-btn:hover{background:#22222b}
#vml-music-btn.on{border-color:#1ee0d0;color:#1ee0d0}
#vml-music-btn svg{width:18px;height:18px}
#vml-music-btn.in-bar{position:relative;right:auto;bottom:auto;z-index:auto;flex:none;width:3.75rem;height:3.75rem;
  border-radius:var(--hy-space-xs,8px);border:1px solid transparent;
  background:var(--hy-color-surface-neutral-panel-inset-emphasis,#1d1d24);box-shadow:inset 0 2px 1px #000}
#vml-music-btn.in-bar svg{width:30px;height:30px}
#vml-music-btn.in-bar.on{border-color:#1ee0d0;box-shadow:inset 0 2px 1px #000,0 0 12px #1ee0d066}
#vml-music-panel{position:fixed;right:16px;bottom:66px;z-index:2147483000;width:280px;padding:14px;
  border-radius:12px;background:#17171d;border:1px solid #2a2a33;color:#e8e8ef;
  font:13px/1.4 Inter,system-ui,sans-serif;box-shadow:0 10px 30px #000a;display:none}
#vml-music-panel.open{display:block}
#vml-music-panel h3{margin:0 0 2px;font-size:14px;font-weight:600}
#vml-music-panel p{margin:0 0 10px;color:#9aa0b4;font-size:12px}
.vml-row{display:flex;align-items:center;justify-content:space-between;padding:7px 0;border-top:1px solid #24242c}
.vml-row span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:190px}
.vml-row small{color:#6f7486;margin-left:6px}
.vml-switch{width:36px;height:20px;border-radius:10px;background:#3a3a46;position:relative;cursor:pointer;flex:none;border:0}
.vml-switch::after{content:"";position:absolute;top:3px;left:3px;width:14px;height:14px;border-radius:50%;background:#e8e8ef;transition:left .15s}
.vml-switch.on{background:#1ee0d0}.vml-switch.on::after{left:19px}
.vml-empty{color:#6f7486;padding:6px 0;border-top:1px solid #24242c}`;

const NOTE_ICON = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
  'stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18V5l12-2v13"/>' +
  '<circle cx="6" cy="18" r="3"/><circle cx="18" cy="16" r="3"/></svg>';

function mountMusicPanel() {
  const style = document.createElement('style');
  style.textContent = MUSIC_CSS;
  const button = document.createElement('button');
  button.id = 'vml-music-btn';
  button.title = 'Send music through the Voicemod mic';
  button.innerHTML = NOTE_ICON;
  const panel = document.createElement('div');
  panel.id = 'vml-music-panel';
  document.head.append(style);
  document.body.append(panel);

  // Sit in Voicemod's floating bar next to its sound square; Vue may re-render the bar, so
  // keep putting the button back. Screens without the bar get the corner button instead.
  const place = () => {
    const toolbar = document.querySelector('.floating-bar__toolbar');
    if (toolbar && button.parentElement !== toolbar) {
      button.classList.add('in-bar');
      toolbar.append(button);
    } else if (!toolbar && button.parentElement !== document.body) {
      button.classList.remove('in-bar');
      document.body.append(button);
    }
  };
  new MutationObserver(place).observe(document.body, { childList: true, subtree: true });
  place();

  // Open the panel just above wherever the button currently is.
  const positionPanel = () => {
    const rect = button.getBoundingClientRect();
    const width = 280;
    panel.style.bottom = `${window.innerHeight - rect.top + 10}px`;
    panel.style.left = `${Math.max(8, Math.min(rect.left + rect.width / 2 - width / 2, window.innerWidth - width - 8))}px`;
    panel.style.right = 'auto';
  };

  let timer = null;
  const render = async () => {
    const { playing, routed } = await ipcRenderer.invoke('music-state');
    const names = new Map(playing.map((n) => [n.toLowerCase(), n]));
    for (const r of routed) if (!names.has(r)) names.set(r, r);
    panel.innerHTML = '<h3>Music through mic</h3><p>Also send these apps\' sound to people hearing your Voicemod mic. ' +
      'You still hear it too.</p>';
    if (!names.size) panel.insertAdjacentHTML('beforeend', '<div class="vml-empty">No apps are playing sound right now.</div>');
    for (const [key, label] of names) {
      const on = routed.includes(key);
      const row = document.createElement('div');
      row.className = 'vml-row';
      row.innerHTML = `<span></span><button class="vml-switch${on ? ' on' : ''}" aria-label="toggle"></button>`;
      row.querySelector('span').textContent = label;
      if (!playing.some((p) => p.toLowerCase() === key)) row.querySelector('span').insertAdjacentHTML('beforeend', '<small>not playing</small>');
      row.querySelector('button').onclick = async () => { await ipcRenderer.invoke('music-set', key, !on); render(); };
      panel.append(row);
    }
    button.classList.toggle('on', routed.length > 0);
  };
  button.onclick = () => {
    const open = panel.classList.toggle('open');
    clearInterval(timer);
    if (open) {
      positionPanel();
      render();
      timer = setInterval(render, 2000);
    }
  };
  render();
}

if (location.protocol === 'vm:' && location.pathname === '/index.html') {
  window.addEventListener('DOMContentLoaded', mountMusicPanel);
}

contextBridge.exposeInMainWorld('launcher', {
  onLine: (cb) => ipcRenderer.on('launcher-line', (_e, line) => cb(line)),
  onFailed: (cb) => ipcRenderer.on('launcher-failed', (_e, message) => cb(message)),
});
