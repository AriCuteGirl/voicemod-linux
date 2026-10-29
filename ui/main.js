// Linux launcher and window for Voicemod: runs first-time setup, starts the Windows engine
// under Wine, serves the frontend extracted from the user's Voicemod.exe and points it at the
// engine's Brigitte websocket.
const { app, BrowserWindow, protocol, net, ipcMain, shell } = require('electron');
const { spawn, spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const sockets = require('net');
const { EngineClient, runEngineCommand, ENGINE_HELP } = require('./engine');
const { pathToFileURL } = require('url');

// Our own files: resources/voicemod in a packaged build, else the repository checkout.
// (Distro Electron builds report isPackaged=true even for `electron .`, so check the files.)
const PACKAGED_RES = path.join(process.resourcesPath, 'voicemod');
const RES = fs.existsSync(path.join(PACKAGED_RES, 'voicemod-linux.sh')) ? PACKAGED_RES : path.resolve(__dirname, '..');
const DATA = process.env.VOICEMOD_HOME ||
  (fs.existsSync(path.join(RES, 'prefix')) ? RES
    : path.join(process.env.XDG_DATA_HOME || path.join(process.env.HOME, '.local/share'), 'voicemod-linux'));
const SCRIPT = path.join(RES, 'voicemod-linux.sh');
const PREFIX = process.env.WINEPREFIX || path.join(DATA, 'prefix');
const FRONTEND = path.join(DATA, 'frontend');
const VM_DATA = path.join(PREFIX, 'drive_c/users', process.env.USER, 'AppData/Local/VoicemodV3');
const ENGINE_LOG = path.join(VM_DATA, 'logs/voicemod-desktop.log');
const DEBUG_LOG = path.join(VM_DATA, 'logs/linux-ui.log');
const CONTROL_API_PORT = 59129;
// voicemod-ctl (installed into ~/.local/bin) talks to this socket; see handleControl().
const CONTROL_SOCKET = path.join(process.env.XDG_RUNTIME_DIR || os.tmpdir(), 'voicemod-linux.sock');

protocol.registerSchemesAsPrivileged([
  { scheme: 'vm', privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: true, stream: true } },
]);

// Environment for the shell script and Wine: drop what the AppImage runtime adds for Electron.
function scriptEnv() {
  const env = { ...process.env, VOICEMOD_HOME: DATA };
  const appdir = process.env.APPDIR;
  for (const key of ['LD_LIBRARY_PATH', 'LD_PRELOAD']) {
    if (!env[key]) continue;
    const kept = env[key].split(':').filter((p) => p && !(appdir && p.startsWith(appdir)));
    if (kept.length) env[key] = kept.join(':');
    else delete env[key];
  }
  return env;
}

function runScript(args, onLine) {
  return new Promise((resolve) => {
    const child = spawn('bash', [SCRIPT, ...args], { env: scriptEnv() });
    let pending = '';
    const feed = (chunk) => {
      pending += chunk.toString().replace(/\r/g, '\n');
      const lines = pending.split('\n');
      pending = lines.pop();
      for (const line of lines) if (line.trim()) onLine?.(line);
    };
    child.stdout.on('data', feed);
    child.stderr.on('data', feed);
    child.on('close', (code) => {
      if (pending.trim()) onLine?.(pending);
      resolve(code);
    });
  });
}

// Wine sockets belong to wineserver, so the engine's ports show up as its listening sockets.
function wineserverPids() {
  const pids = [];
  for (const pid of fs.readdirSync('/proc').filter((p) => /^\d+$/.test(p))) {
    try {
      if (fs.readFileSync(`/proc/${pid}/comm`, 'utf8').trim() !== 'wineserver') continue;
      const env = fs.readFileSync(`/proc/${pid}/environ`, 'utf8').split('\0');
      const prefix = env.find((e) => e.startsWith('WINEPREFIX='));
      if (!prefix || path.resolve(prefix.slice(11)) === path.resolve(PREFIX)) pids.push(pid);
    } catch { /* process vanished or not ours */ }
  }
  return pids;
}

function socketInodes(pid) {
  const inodes = new Set();
  for (const fd of fs.readdirSync(`/proc/${pid}/fd`)) {
    try {
      const m = /^socket:\[(\d+)\]$/.exec(fs.readlinkSync(`/proc/${pid}/fd/${fd}`));
      if (m) inodes.add(m[1]);
    } catch { /* fd closed meanwhile */ }
  }
  return inodes;
}

// Brigitte listens on a random 127.0.0.1 port; the Control API is the fixed 59129.
function findBrigittePorts() {
  const inodes = new Set();
  for (const pid of wineserverPids()) for (const i of socketInodes(pid)) inodes.add(i);
  const ports = [];
  for (const line of fs.readFileSync('/proc/net/tcp', 'utf8').split('\n').slice(1)) {
    const f = line.trim().split(/\s+/);
    if (f.length < 10 || f[3] !== '0A' || !inodes.has(f[9])) continue; // 0A = LISTEN
    const [addr, port] = f[1].split(':');
    const n = parseInt(port, 16);
    if (addr === '0100007F' && n !== CONTROL_API_PORT) ports.push(n);
  }
  return ports;
}

function readDeviceId() {
  try {
    const m = /DeviceId ([0-9a-f-]{36})/.exec(fs.readFileSync(ENGINE_LOG, 'utf8'));
    return m ? m[1] : 'unknown';
  } catch {
    return 'unknown';
  }
}

function readVersion() {
  try {
    return fs.readFileSync(path.join(DATA, '.voicemod-version'), 'utf8').trim();
  } catch {
    return 'unknown';
  }
}

async function waitForBrigitte(timeoutMs) {
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    const ports = findBrigittePorts();
    if (ports.length) return ports[0];
    await new Promise((r) => setTimeout(r, 500));
  }
  return null;
}

function pageUrl(port) {
  const query = new URLSearchParams({
    t: String(Date.now()),
    boot: 'true',
    os: 'Windows',
    deviceId: readDeviceId(),
    version: readVersion(),
    lang: process.env.VOICEMOD_LANG || 'en',
  });
  return `vm://app/index.html?${query}#/h/${port}/voices`;
}

const isSetUp = () => fs.existsSync(path.join(FRONTEND, 'index.html')) &&
  fs.existsSync(path.join(VM_DATA, 'app/Voicemod.exe'));

const linkFromArgs = (argv) => argv.find((a) => a.startsWith('voicemod:'));

let win = null;
let wheel = null;
let wheelOpen = false;
let wheelHideTimer = null;
let engineStarted = false;
let brigittePort = null;

function status(text) {
  win?.webContents.send('launcher-line', text);
}

async function forwardLink(url) {
  if (!engineStarted) return;
  await runScript(['url', url]);
  win?.show();
  win?.focus();
}

// The Action Wheel lives in its own transparent window that stays loaded and connected, so
// opening it is instant. Hyprland rules (title "Voicemod Wheel") make it a pinned overlay.
function createWheel(port) {
  wheel = new BrowserWindow({
    show: false,
    frame: false,
    transparent: true,
    backgroundColor: '#00000000',
    alwaysOnTop: true,
    skipTaskbar: true,
    resizable: false,
    hasShadow: false,
    fullscreenable: false,
    width: 1920,
    height: 1080,
    title: 'Voicemod Wheel',
    webPreferences: {
      preload: path.join(__dirname, 'wheel-preload.js'),
      contextIsolation: false,
      sandbox: false,
    },
  });
  wheel.setAlwaysOnTop(true, 'screen-saver');
  wheel.on('page-title-updated', (e) => e.preventDefault());
  const query = new URLSearchParams({
    port: String(port),
    t: String(Date.now()),
    deviceId: readDeviceId(),
    appVersion: readVersion(),
    env: 'production',
  });
  wheel.loadURL(`vm://app/wheel/index.html?${query}`);
  wheel.on('closed', () => { wheel = null; wheelOpen = false; });
}

function hideWheel() {
  clearTimeout(wheelHideTimer);
  wheelOpen = false;
  wheel?.hide();
}

function showWheel() {
  if (!wheel || wheelOpen) return;
  wheelOpen = true;
  wheel.show();
  wheel.focus();
  wheel.webContents.send('wheel-show');
}

function releaseWheel() {
  if (!wheel || !wheelOpen) return;
  wheel.webContents.send('wheel-release');
  // The page reports WheelHidden after its closing animation; don't stay up if it doesn't.
  clearTimeout(wheelHideTimer);
  wheelHideTimer = setTimeout(hideWheel, 1500);
}

const CONTROL_HELP = `commands:\nwheel show|release|toggle\nshow\n${ENGINE_HELP}`;
let engineClient = null;

async function handleControl(line) {
  const [cmd, ...rest] = line.trim().split(/\s+/);
  const arg = rest[0];
  if (cmd === 'wheel') {
    if (!wheel) return 'error: engine not ready';
    if (arg === 'show') showWheel();
    else if (arg === 'release' || arg === 'hide') releaseWheel();
    else if (arg === 'toggle' || !arg) (wheelOpen ? releaseWheel : showWheel)();
    else return `error: ${CONTROL_HELP}`;
    return 'ok';
  }
  if (cmd === 'show') {
    win?.show();
    win?.focus();
    return 'ok';
  }
  if (!brigittePort) return 'error: engine not ready';
  engineClient ??= new EngineClient(brigittePort);
  try {
    const reply = await runEngineCommand(engineClient, cmd, rest);
    if (reply !== null) return reply;
  } catch (err) {
    return `error: ${err.message}`;
  }
  return `error: ${CONTROL_HELP}`;
}

function startControlServer() {
  try { fs.unlinkSync(CONTROL_SOCKET); } catch { /* not there */ }
  const server = sockets.createServer((conn) => {
    let buffer = '';
    conn.on('data', async (chunk) => {
      buffer += chunk;
      if (!buffer.includes('\n')) return;
      const reply = await handleControl(buffer.split('\n')[0]);
      conn.end(String(reply) + '\n');
    });
  });
  server.listen(CONTROL_SOCKET, () => fs.chmodSync(CONTROL_SOCKET, 0o600));
  app.on('will-quit', () => { try { fs.unlinkSync(CONTROL_SOCKET); } catch { /* gone */ } });
}

// Put voicemod-ctl on PATH so compositor binds can reach the running app.
function installCtl() {
  const src = path.join(RES, 'tools', 'voicemod-ctl');
  const dest = path.join(os.homedir(), '.local/bin/voicemod-ctl');
  try {
    const content = fs.readFileSync(src);
    if (fs.existsSync(dest) && fs.readFileSync(dest).equals(content)) return;
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.writeFileSync(dest, content, { mode: 0o755 });
  } catch (err) {
    console.error(`could not install voicemod-ctl: ${err.message}`);
  }
}

async function launch() {
  win = new BrowserWindow({
    width: 1280,
    height: 800,
    minWidth: 960,
    minHeight: 600,
    title: 'Voicemod',
    backgroundColor: '#0d0d12',
    autoHideMenuBar: true,
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true },
  });
  // The hidden wheel window would otherwise keep the app alive after the main window closes.
  win.on('closed', () => app.quit());
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (/^https?:\/\//.test(url)) shell.openExternal(url);
    return { action: 'deny' };
  });
  if (process.env.VOICEMOD_UI_LOG) {
    win.webContents.on('console-message', (e) => console.log(`[page] ${e.message}`));
  }
  await win.loadFile(path.join(__dirname, 'launcher.html'));

  if (!isSetUp()) {
    status('First start: setting up Voicemod. This downloads about 240 MB and takes a few minutes.');
    const code = await runScript(['setup'], status);
    if (code !== 0 || !isSetUp()) {
      win.webContents.send('launcher-failed', `Setup failed (exit ${code}). See the log above.`);
      return;
    }
  }

  status('Starting the Voicemod engine…');
  if (await runScript(['start-engine'], status) !== 0) {
    win.webContents.send('launcher-failed', 'Could not start the engine. See the log above.');
    return;
  }
  engineStarted = true;

  const port = await waitForBrigitte(90000);
  if (!port) {
    win.webContents.send('launcher-failed', 'The engine did not come up within 90 seconds.');
    return;
  }
  const pending = linkFromArgs(process.argv);
  if (pending) forwardLink(pending);
  brigittePort = port;
  win.loadURL(pageUrl(port));
  createWheel(port);
}

if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', (_e, argv) => {
    const link = linkFromArgs(argv);
    if (link) forwardLink(link);
    if (win) {
      if (win.isMinimized()) win.restore();
      win.show();
      win.focus();
    }
  });

  app.whenReady().then(() => {
    protocol.handle('vm', (request) => {
      const rel = decodeURIComponent(new URL(request.url).pathname);
      const file = path.resolve(FRONTEND, '.' + rel);
      if (!file.startsWith(FRONTEND + path.sep)) return new Response('forbidden', { status: 403 });
      return net.fetch(pathToFileURL(file).toString());
    });

    ipcMain.handle('debug-log', (_e, text) => fs.promises.appendFile(DEBUG_LOG, text));
    ipcMain.handle('open-url', (_e, url) => {
      if (/^https?:\/\//.test(url)) shell.openExternal(url);
    });
    ipcMain.on('wheel-closed', () => setTimeout(hideWheel, 200));

    installCtl();
    startControlServer();
    launch();
  });

  app.on('window-all-closed', () => app.quit());

  // Closing the window ends the session: stop Wine, the watcher and the virtual devices.
  app.on('will-quit', () => {
    if (engineStarted && !process.env.VOICEMOD_KEEP_ENGINE) {
      spawnSync('bash', [SCRIPT, 'stop'], { env: scriptEnv(), timeout: 15000 });
    }
  });
}
