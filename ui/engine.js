// Direct client for the engine's Brigitte websocket, used by voicemod-ctl commands.
// Speaks the same "busMessage" protocol as Voicemod's frontend, with the same commands
// its buttons and Action Wheel send (selectVoice, playSound, enableVoiceChanger, ...).

const RESOURCE_TTL_MS = 60000;
const REPLY_TIMEOUT_MS = 4000;

// Each toggle: the command that sets it, and the query + event that report its state.
const TOGGLES = {
  'voice-changer': { set: 'enableVoiceChanger', query: 'isVoiceChangerEnabled', manual: true },
  'hear-myself': { set: 'setHearMyself', query: 'getHearMyself', manual: true },
  mute: { set: 'EnableMuteMic', query: 'IsMuteMicEnabled', manual: false },
};

class EngineClient {
  constructor(port) {
    this.port = port;
    this.ws = null;
    this.waiters = [];
    this.resources = null;
    this.resourcesAt = 0;
  }

  connect() {
    if (this.ws && this.ws.readyState <= WebSocket.OPEN) return this.ready;
    this.ws = new WebSocket(`ws://127.0.0.1:${this.port}`);
    this.ready = new Promise((resolve, reject) => {
      this.ws.onopen = resolve;
      this.ws.onerror = () => reject(new Error('cannot reach the Voicemod engine'));
    });
    this.ws.onmessage = (msg) => {
      let data;
      try { data = JSON.parse(msg.data)?.bridgeData?.data; } catch { return; }
      if (!data) return;
      const matched = this.waiters.filter((w) => w.type === data.type);
      this.waiters = this.waiters.filter((w) => w.type !== data.type);
      for (const w of matched) w.resolve(data.object ?? {});
    };
    this.ws.onclose = () => { this.ws = null; };
    return this.ready;
  }

  async send(type, object = null) {
    await this.connect();
    this.ws.send(JSON.stringify({ bridgeData: { type: 'busMessage', data: { id: crypto.randomUUID(), type, object } } }));
  }

  async request(type, object, replyType = type) {
    const reply = new Promise((resolve, reject) => {
      const waiter = { type: replyType, resolve };
      this.waiters.push(waiter);
      setTimeout(() => {
        this.waiters = this.waiters.filter((w) => w !== waiter);
        reject(new Error(`no reply to ${type}`));
      }, REPLY_TIMEOUT_MS);
    });
    await this.send(type, object);
    return reply;
  }

  async allResources() {
    if (this.resources && Date.now() - this.resourcesAt < RESOURCE_TTL_MS) return this.resources;
    const { collections = [] } = await this.request('GetAllResources', { sender: 'wheel' });
    const byId = new Map();
    for (const collection of collections) {
      for (const r of collection.resources ?? []) byId.set(r.id, { id: r.id, name: r.name, type: r.type, locked: r.locked });
    }
    this.resources = [...byId.values()];
    this.resourcesAt = Date.now();
    return this.resources;
  }

  // Exact name first, then prefix, then substring; locked (Pro-only) items are skipped.
  async find(type, query) {
    const q = query.toLowerCase();
    const usable = (await this.allResources()).filter((r) => r.type === type && !r.locked);
    return usable.find((r) => r.name.toLowerCase() === q) ||
      usable.find((r) => r.name.toLowerCase().startsWith(q)) ||
      usable.find((r) => r.name.toLowerCase().includes(q));
  }

  async list(type) {
    const names = (await this.allResources()).filter((r) => r.type === type && !r.locked).map((r) => r.name);
    return [...new Set(names)].sort((a, b) => a.localeCompare(b)).join('\n');
  }

  async toggle(name, mode) {
    const t = TOGGLES[name];
    let enabled;
    if (mode === 'on') enabled = true;
    else if (mode === 'off') enabled = false;
    else enabled = !(await this.request(t.query)).enabled;
    await this.send(t.set, { enabled, manual: t.manual });
    return `${name} ${enabled ? 'on' : 'off'}`;
  }
}

const HELP = [
  'voice NAME | voice random | voice list',
  'sound NAME | sound stop | sound list',
  'voice-changer on|off|toggle, hear-myself on|off|toggle, mute on|off|toggle',
].join('\n');

// Runs one voicemod-ctl command line ("voice Banana", "mute toggle", ...); returns the reply.
async function runEngineCommand(client, cmd, rest) {
  const arg = rest.join(' ').trim();
  if (cmd === 'voice') {
    if (arg === 'list') return client.list('voice');
    if (arg === 'random') { await client.send('selectRandomVoice'); return 'ok'; }
    const voice = arg && await client.find('voice', arg);
    if (!voice) return `error: no unlocked voice matches "${arg}"`;
    await client.send('selectVoice', { id: voice.id });
    return `voice ${voice.name}`;
  }
  if (cmd === 'sound') {
    if (arg === 'list') return client.list('sound');
    if (arg === 'stop') { await client.send('stopAllSounds'); return 'ok'; }
    const sound = arg && await client.find('sound', arg);
    if (!sound) return `error: no unlocked sound matches "${arg}"`;
    await client.send('playSound', { id: sound.id, sendToVAD: true });
    return `sound ${sound.name}`;
  }
  if (TOGGLES[cmd]) {
    if (!['on', 'off', 'toggle', ''].includes(arg)) return `error: ${cmd} on|off|toggle`;
    return client.toggle(cmd, arg || 'toggle');
  }
  return null;
}

module.exports = { EngineClient, runEngineCommand, ENGINE_HELP: HELP };
