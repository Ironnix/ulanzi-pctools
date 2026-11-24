// PC Tools — Ulanzi Studio plugin (one plugin = one Node process + one tiny helper).
//   media keys  : playpause/next/previous/stop/mute/volup/voldown (no key presses)
//   micmute     : toggle default microphone mute, key shows live state
//   audioswitch : switch default output between two devices (e.g. headset <-> speakers)
//   terminal    : open a cmd window running a command, e.g. ssh user@host
//   phone       : stream phone audio to the PC over Bluetooth (AudioPlaybackConnection), toggle on/off
// Everything goes through pchelper.exe (Windows APIs, no emulated key presses).
// Status is event-driven: Windows notifies the helper on changes, nothing polls.
import UlanziApi from '../sdk/plugin-common-node/index.js';
import { spawn } from 'child_process';
import path from 'path';
import fs from 'fs';

const PLUGIN_UUID = 'com.ulanzi.ulanzistudio.pctools';
const pluginDir = path.resolve(path.dirname(process.argv[1]), '..');
const HELPER_NAME = typeof __HELPER_NAME__ !== 'undefined' ? __HELPER_NAME__ : 'pchelper.exe'; // versioned: a running (locked) older helper never blocks an update
const helperExe = path.join(pluginDir, 'plugin', HELPER_NAME);

// remove helpers left over from older versions (they're no longer running once Ulanzi restarted)
try {
  for (const f of fs.readdirSync(path.join(pluginDir, 'plugin')))
    if (/^pchelper.*\.exe$/i.test(f) && f !== HELPER_NAME) {
      try { fs.unlinkSync(path.join(pluginDir, 'plugin', f)); } catch {}
    }
} catch {}

const $UD = new UlanziApi();

// ---------------------------------------------------------------- helper process
let helper = null;
let queue = []; // pending requests: { multi, lines, resolve }
let buf = '';

function startHelper() {
  const proc = spawn(helperExe, [], { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
  helper = proc;
  queue = [];
  buf = '';
  proc.stdout.on('data', (chunk) => {
    buf += chunk.toString('utf8');
    let i;
    while ((i = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, i).replace(/\r$/, '');
      buf = buf.slice(i + 1);
      onHelperLine(line);
    }
  });
  proc.stderr.on('data', (d) => console.warn('[pctools helper stderr]', d.toString()));
  const dead = (why) => {
    console.warn('[pctools] helper stopped:', why);
    if (helper === proc) helper = null;
    queue.forEach((q) => { clearTimeout(q.timer); q.resolve(q.multi ? q.lines : null); });
    queue = [];
  };
  proc.on('error', (e) => dead(e.message));
  proc.on('exit', (code) => dead('exit ' + code));
}

function onHelperLine(line) {
  if (line === 'READY' || !line) return;
  if (line.startsWith('EVENT ')) return onEvent(line.slice(6));
  const q = queue[0];
  if (!q) return console.log('[pctools helper]', line);
  if (q.multi) {
    if (line === 'END') { queue.shift(); clearTimeout(q.timer); q.resolve(q.lines); }
    else if (line.startsWith('ERR')) { console.warn('[pctools helper]', line); queue.shift(); clearTimeout(q.timer); q.resolve(q.lines); }
    else q.lines.push(line);
  } else {
    queue.shift();
    clearTimeout(q.timer);
    if (line.startsWith('ERR')) console.warn('[pctools helper]', line);
    q.resolve(line);
  }
}

function request(cmd, multi = false) {
  if (!helper || helper.exitCode !== null || !helper.stdin.writable) startHelper();
  return new Promise((resolve) => {
    const q = { multi, lines: [], resolve };
    // safety net: if the helper never answers, drop this request instead of blocking everything behind it
    q.timer = setTimeout(() => {
      const i = queue.indexOf(q);
      if (i >= 0) queue.splice(i, 1);
      console.warn('[pctools] no reply to', cmd);
      resolve(multi ? q.lines : null);
    }, 20000);
    queue.push(q);
    helper.stdin.write(cmd + '\n');
  });
}

async function listOutputs() {
  const lines = (await request('list', true)) || [];
  const devices = [];
  let current = '';
  for (const l of lines) {
    if (l.startsWith('DEFAULT ')) current = l.slice(8);
    else if (l.startsWith('DEV ')) {
      const [id, name] = l.slice(4).split('\t');
      devices.push({ id, name });
    }
  }
  return { devices, current };
}

async function getStatus() {
  const line = await request('status');
  const m = line && line.match(/^STATUS mic=(-?\d) out=(.*)$/);
  return m ? { mic: Number(m[1]), out: m[2] } : null;
}

// ---------------------------------------------------------------- key instances
const keys = new Map(); // context -> { action, settings, shownState }

const actionOf = (msg) => String(msg.uuid || '').split('.').pop();

function setState(ctx, state) {
  const k = keys.get(ctx);
  if (!k || k.shownState === state) return;
  k.shownState = state;
  $UD.setStateIcon(ctx, state);
}

const status = { mic: -1, out: '', phone: 0 };

function paint() {
  for (const [ctx, k] of keys) {
    if (k.action === 'micmute') setState(ctx, status.mic === 1 ? 1 : 0);
    if (k.action === 'phone') setState(ctx, status.phone === 1 ? 1 : 0);
    if (k.action === 'audioswitch') {
      if (status.out && status.out === k.settings.devA) setState(ctx, 0);
      else if (status.out && status.out === k.settings.devB) setState(ctx, 1);
      else setState(ctx, 2); // some other device is active
    }
  }
}

function onEvent(ev) {
  if (ev.startsWith('mic=')) status.mic = Number(ev.slice(4));
  else if (ev.startsWith('out=')) status.out = ev.slice(4);
  else if (ev.startsWith('phone=')) status.phone = Number(ev.slice(6));
  paint();
}

async function refresh() {
  const s = await getStatus();
  if (s) Object.assign(status, s);
  paint();
}

function upsert(msg) {
  const ctx = msg.context;
  const k = keys.get(ctx) || { action: actionOf(msg), settings: {}, shownState: -1 };
  if (msg.param && typeof msg.param === 'object') k.settings = { ...k.settings, ...msg.param };
  keys.set(ctx, k);
  k.shownState = -1; // force icon refresh
  if (k.action === 'micmute' || k.action === 'audioswitch') refresh().catch(() => {});
}

// ---------------------------------------------------------------- actions
const MEDIA = ['playpause', 'next', 'previous', 'stop', 'mute', 'volup', 'voldown'];

async function runAction(ctx, k) {
  if (MEDIA.includes(k.action)) {
    const r = await request('media ' + k.action);
    if (!r || !r.startsWith('OK')) $UD.showAlert(ctx);
    return;
  }

  if (k.action === 'micmute') {
    const r = await request('mic toggle');
    if (!r || !r.startsWith('OK')) return $UD.showAlert(ctx);
    return onEvent(r.slice(3)); // OK mic=1 -> instant icon update
  }

  if (k.action === 'audioswitch') {
    const { devA, devB } = k.settings;
    if (!devA || !devB) { $UD.toast('PC Tools: pick both audio devices in the key settings first'); return $UD.showAlert(ctx); }
    const target = status.out === devA ? devB : devA;
    const r = await request('setout ' + target);
    if (!r || !r.startsWith('OK')) return $UD.showAlert(ctx);
    return onEvent(r.slice(3)); // OK out=<id>
  }

  if (k.action === 'phone') {
    const id = (k.settings.phoneId || '').trim();
    const r = await request(('phone toggle ' + id).trim());
    if (!r || !r.startsWith('OK')) { if (r) $UD.toast('Phone audio: ' + r.replace(/^ERR phone /, '')); return $UD.showAlert(ctx); }
    return onEvent(r.slice(3)); // OK phone=1
  }

  if (k.action === 'terminal') {
    const command = (k.settings.command || '').trim();
    if (!command) { $UD.toast('SSH / Terminal: enter a command in the key settings first'); return $UD.showAlert(ctx); }
    const title = (k.settings.title || 'Terminal').replace(/"/g, '');
    // `start` opens a new visible console window; /k keeps it open after the command ends.
    const child = spawn('cmd.exe', [`/c start "${title}" cmd.exe /k ${command}`], {
      windowsVerbatimArguments: true,
      windowsHide: true,
      detached: true,
      stdio: 'ignore',
    });
    child.on('error', () => $UD.showAlert(ctx));
    child.unref();
  }
}

// ---------------------------------------------------------------- SDK wiring
$UD.connect(PLUGIN_UUID);

$UD.onConnected(() => { if (!helper) startHelper(); });
$UD.on('error', (e) => console.warn('[pctools] socket error', e));
$UD.onAdd((msg) => upsert(msg));
$UD.onParamFromApp((msg) => upsert(msg));
$UD.onParamFromPlugin((msg) => upsert(msg));

$UD.onRun((msg) => {
  if (!keys.has(msg.context)) upsert(msg);
  runAction(msg.context, keys.get(msg.context)).catch((e) => {
    console.warn('[pctools]', e);
    $UD.showAlert(msg.context);
  });
});

$UD.onClear((msg) => {
  for (const p of msg.param || []) keys.delete(p.context);
});

// Property inspector asks for the device list
$UD.onSendToPlugin(async (msg) => {
  const payload = msg.payload || {};
  if (payload.request === 'phones') {
    const lines = (await request('phones', true)) || [];
    const phones = lines.filter((l) => l.startsWith('PHONE ')).map((l) => {
      const [id, name] = l.slice(6).split('\t');
      return { id, name };
    });
    $UD.sendToPropertyInspector({ phones }, msg.context);
  }
  if (payload.request === 'devices') {
    const { devices, current } = await listOutputs();
    $UD.sendToPropertyInspector({ devices, current }, msg.context);
  }
});

process.on('exit', () => { try { helper && helper.kill(); } catch {} });
