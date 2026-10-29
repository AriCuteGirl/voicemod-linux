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

contextBridge.exposeInMainWorld('launcher', {
  onLine: (cb) => ipcRenderer.on('launcher-line', (_e, line) => cb(line)),
  onFailed: (cb) => ipcRenderer.on('launcher-failed', (_e, message) => cb(message)),
});
