/**
 * P4 Player web UI dev server — the device's API, faked well enough to work on pages without a
 * panel. Usage: npm run dev → http://localhost:5000
 *
 * State: dev/state.json, a real /api/state of the player (Bluetooth playing) with
 * names and addresses replaced. Writes to /api/settings land in it and go out over /ws as the
 * panel does; *.cmd keys are acted on roughly (play/pause/stop), never stored. Service's endpoints
 * (log levels, bindings, the screenshot, factory reset) answer as the device's do, and so do these:
 * the card's library, the DLNA browser (its servers appear 2.5 s after a search), the cover.
 * Pages: data/ui.json, as the build generated it. Languages: src/i18n/<lang>.json at /lang/.
 */
import { createServer } from 'http';
import { readFileSync, existsSync } from 'fs';
import { join, extname } from 'path';
import { fileURLToPath } from 'url';
import { spawn } from 'child_process';
import { WebSocketServer } from 'ws';

const __dirname = fileURLToPath(new URL('.', import.meta.url));
const PORT = Number(process.env.PORT) || 5000;

const fixture = JSON.parse(readFileSync(join(__dirname, 'dev', 'state.json'), 'utf8'));
delete fixture._;
const state = { ...fixture };

// The station list as GET/POST /api/stations keep it (modules/radio/src/stations_http.inc):
// the generation moves by two with every change and goes out as radio.list_rev.
const stations = {
  rev: 2,
  list: {
    stations: [
      { name: 'SomaFM Groove Salad', url: 'http://ice1.somafm.com/groovesalad-128-mp3' },
      { name: 'SomaFM Drone Zone', url: 'http://ice1.somafm.com/dronezone-128-mp3' },
      { name: 'SomaFM Deep Space', url: 'http://ice1.somafm.com/deepspaceone-128-mp3' },
      { name: 'Radio Paradise Main', url: 'http://stream.radioparadise.com/mp3-128' },
      { name: 'Radio Paradise Mellow', url: 'http://stream.radioparadise.com/mellow-128' },
      { name: 'Nightride FM', url: 'http://stream.nightride.fm/nightride.mp3' },
    ],
    presets: [0, 1, 2, 3, 4, 5],
  },
};
state['radio.list_rev'] = stations.rev;
const auth = { enabled: false, username: 'admin', pass: 'modesp' };

// The EQ as GET/POST /api/eq keep it (modules/audio/src/eq_http.inc, audio_dsp/eq_store.h). The
// presets are read out of the firmware's own table, so the page's dropdown is the panel's.
const PRESET_SRC = readFileSync(join(__dirname, '..', 'components', 'audio_dsp', 'src', 'eq_presets.cpp'), 'utf8');
const PRESETS = [{ id: 'custom', g: null }].concat(
  [...PRESET_SRC.matchAll(/\{ "(\w+)", "[^"]*", \{ ([-\d., f]+) \} \}/g)].map((m) => ({
    id: m[1],
    g: m[2].split(',').map((x) => parseFloat(x)),
  })));
// Offered: the table's first EQ_PRESET_SHOWN, and a kept one while it plays (eq_offered).
const PRESET_SHOWN = Number(readFileSync(join(__dirname, '..', 'components', 'audio_dsp', 'include', 'eq_presets.h'), 'utf8')
  .match(/EQ_PRESET_SHOWN = (\d+)/)[1]);
function offered(active) {
  const ids = PRESETS.slice(0, PRESET_SHOWN).map((p) => p.id);
  return ids.includes(active) ? ids : ids.concat(active);
}
const PRESET_LAYOUT = [['lowshelf', 40, 0.707], ...[62.5, 125, 250, 500, 1000, 2000, 4000, 8000].map((f) => ['peak', f, 1]), ['highshelf', 12000, 0.707]];
const flat = [80, 125, 250, 500, 1000, 2000, 4000, 8000, 12000, 16000].map((f) => ({ type: 'peak', f, gain: 0, q: 1, on: false }));
const eq = { rev: 1, preset: 'custom', bands: flat, custom: flat };
state['audio.eq_rev'] = eq.rev;
function eqPreset(id) {
  const p = PRESETS.find((x) => x.id === id);
  if (!p || !p.g) return eq.custom;
  return PRESET_LAYOUT.map(([type, f, q], i) => ({ type, f, q, gain: p.g[i], on: Math.abs(p.g[i]) >= 0.05 }));
}
// The satellite's Bluetooth settings as GET/POST /api/bt keep them (modules/bt/src/bt_http.inc).
// /dev/pair?kind=0|1|2 starts a pairing request as a phone would; /dev/pair_panel answers it as
// the panel would.
const bt = {
  linked: true, settings: true, known: true, radio: true, discoverable: true, auto_reconnect: true,
  codecs_supported: 0x1f, codecs_enabled: 0x0b, name: 'P4 Player', total: 2,
  bonds: [
    { addr: 'AA:BB:CC:00:11:22', name: 'Pixel 8', connected: true, last: true },
    { addr: 'AA:BB:CC:33:44:55', name: 'iPad', connected: false, last: false },
  ],
  pairing: { seq: 0, active: false, kind: 0, name: '', code: 0, result: 255, by: 0, accepted: false },
  confirmer: true,
};
let btRev = 1;
state['bt.cfg_rev'] = btRev;
function btPairKeys() {
  const p = bt.pairing;
  const d = { 'bt.pair_seq': p.seq, 'bt.pair_active': p.active, 'bt.pair_kind': p.kind, 'bt.pair_name': p.name,
              'bt.pair_by': p.by, 'bt.pair_result': p.result };
  Object.assign(state, d);
  broadcast(d);
}
function btMoved() {
  btRev++;
  state['bt.cfg_rev'] = btRev;
  setTimeout(() => broadcast({ 'bt.cfg_rev': btRev }), 300);
}
function btFinish(accept) {
  setTimeout(() => { bt.pairing.active = false; bt.pairing.result = accept ? 0 : 1; btPairKeys(); }, 1500);
}

// Wi-Fi as POST /api/wifi, GET /api/wifi/scan and GET/POST /api/wifi/ap answer it
// (http_service.cpp, WiFiService::try_connect). A join drops the WebSocket after 1.5 s, as the
// radio leaving the network does, and lets it back with the outcome in wifi.try 4.5 s later.
// The password that works is `pass` below; an open network always works. /dev/wifi?scan=fail
// makes the next scans fail, ?scan=ok undoes it.
const wifi = {
  nets: [
    { ssid: 'home', rssi: -48, auth: 'WPA2', pass: 'homepass1' },
    { ssid: 'home', rssi: -71, auth: 'WPA2', pass: 'homepass1' },
    { ssid: 'Сусіди', rssi: -63, auth: 'WPA2/WPA3', pass: '12345678' },
    { ssid: 'Guest', rssi: -70, auth: 'open' },
    { ssid: 'Mesh-7F', rssi: -74, auth: 'WPA2/WPA3', pass: 'meshmesh' },
    { ssid: 'Cafe "Kava" \\ 5G', rssi: -76, auth: 'WPA2', pass: 'kava"\\pass' },
    { ssid: 'Corp', rssi: -79, auth: 'WPA2-Enterprise' },
    { ssid: 'Office', rssi: -77, auth: 'WPA3' },
    { ssid: 'TP-Link_OLD', rssi: -82, auth: 'WEP' },
    { ssid: 'Printer', rssi: -86, auth: 'WPA' },
  ],
  ap: { ssid: 'ModESP-0000', channel: 6, pass: 'appass12' },
  down: false,
  scanFail: false,
};
Object.assign(state, { 'wifi.try': '', 'wifi.try_ssid': '' });
function wifiSet(d) {
  Object.assign(state, d);
  broadcast(d);
}
function wifiJoin(ssid, password) {
  wifiSet({ 'wifi.try': 'connecting', 'wifi.try_ssid': ssid });
  setTimeout(() => {
    wifi.down = true;
    for (const ws of clients) ws.close();
  }, 1500);
  setTimeout(() => {
    const n = wifi.nets.find((x) => x.ssid === ssid);
    const ok = n && (n.auth === 'open' || n.pass === password);
    if (ok) {
      Object.assign(state, { 'wifi.connected': true, 'wifi.mode': 'sta', 'wifi.ssid': ssid, 'wifi.ip': '192.168.1.77', 'wifi.try': 'ok' });
    } else {
      Object.assign(state, { 'wifi.reason': n ? 15 : 201, 'wifi.try': 'fail' });
    }
    wifi.down = false;
  }, 6000);
}

// Service. The console's log levels as GET/POST /api/loglevel keep them (log_policy.cpp): a
// "*" puts the kept tags back at INFO. The calibration tone turns itself off after 60 s, and the
// port measurement walks its statuses in ~15 s, as audio_module.cpp does. The diagnostics move a
// little every 2 s without a WebSocket delta, as on the device: only /api/state shows them.
const LOG_LEVELS = ['none', 'error', 'warn', 'info', 'debug', 'verbose'];
const log = { global: 'warn', shipping_default: 'warn', ceiling: 'info',
              kept: ['main', 'ConfigSvc', 'UserStore', 'I2sStd', 'Audio', 'Radio', 'Bt', 'sdmmc_card'].map((tag) => ({ tag, level: 'info' })) };
let toneMs = 0;
let meter = null;
function quiet(d) { Object.assign(state, d); }
function loud(d) { Object.assign(state, d); broadcast(d); }
function portMeasure() {
  if (meter) return;
  const was = { src: state['audio.source'] ?? 0, mode: state['audio.tone_mode'] ?? 0, hz: state['audio.tone_hz'] ?? 1000 };
  // the device's own messages ("@key", arguments behind U+001F; audio_module.cpp port_meter_loop)
  const U = '\x1f';
  const steps = [
    [0, { 'audio.port_meas_status': '@cal.m_silence', 'audio.source': 0 }],
    [4000, { 'audio.tone_mode': 1, 'audio.tone_hz': 90, 'audio.source': 1, 'audio.port_meas_status': `@cal.m_progress${U}hz=20${U}k=1${U}n=41` }],
    [8000, { 'audio.tone_hz': 45, 'audio.port_meas_status': `@cal.m_progress${U}hz=45${U}k=21${U}n=41` }],
    [12000, { 'audio.tone_hz': 80, 'audio.port_meas_status': `@cal.m_progress${U}hz=80${U}k=36${U}n=41` }],
  ];
  meter = steps.map(([ms, d]) => setTimeout(() => loud(d), ms));
  meter.push(setTimeout(() => {
    meter = null;
    // found and offered, not written: «Прийняти» or «Застосувати» writes audio.port_hz
    loud({ 'audio.tone_mode': was.mode, 'audio.tone_hz': was.hz, 'audio.source': was.src, 'audio.port_meas': false,
           'audio.port_meas_hz': 63.4, 'audio.port_meas_status': `@cal.m_found${U}hz=63` });
  }, 15000));
}
function portMeasureStop() {
  if (!meter) return;
  meter.forEach(clearTimeout);
  meter = null;
  loud({ 'audio.source': 0, 'audio.tone_mode': 0, 'audio.port_meas_status': '@cal.m_stopped' });
}
// a BMP of the glass, 800×480 as the rotated panel sends it: the scheme's surface and an amber bar
function screenshotBmp() {
  const w = 800, h = 480, stride = w * 3;
  const buf = Buffer.alloc(54 + stride * h);
  buf.write('BM', 0);
  buf.writeUInt32LE(buf.length, 2);
  buf.writeUInt32LE(54, 10);
  buf.writeUInt32LE(40, 14);
  buf.writeInt32LE(w, 18);
  buf.writeInt32LE(-h, 22);
  buf.writeUInt16LE(1, 26);
  buf.writeUInt16LE(24, 28);
  buf.writeUInt32LE(stride * h, 34);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const o = 54 + y * stride + x * 3;
      const bar = y > 380 && y < 400 && x > 40 && x < 40 + 720 * ((state['audio.volume'] ?? 30) / 100);
      const card = x > 40 && x < 760 && y > 60 && y < 340;
      const [r, g, b] = bar ? [0xff, 0xb9, 0x5f] : card ? [0x23, 0x1f, 0x18] : [0x17, 0x13, 0x0d];
      buf[o] = b; buf[o + 1] = g; buf[o + 2] = r;
    }
  }
  return buf;
}

// The card's index as GET /api/library pages it (modules/ui_lvgl/src/ui_web_media.cpp), the
// DLNA browser as GET /api/dlna/browser shows it and dlna.browse_cmd drives it (components/
// dlna_client: twelve rows a page, a revision on every snapshot, a command for another revision
// ignored), and the Player's picture as GET /api/cover sends it, named by ui.cover.
const library = { gen: 2, root: '/sdcard', rows: [] };
for (const [artist, albums] of [
  ['ДахаБраха', ['Шлях', 'Night in the Ukrainian Steppes']],
  ['Океан Ельзи', ['Земля', 'Без меж']],
  ['Miles Davis', ['Kind of Blue']],
  ['Daft Punk', ['Discovery', 'Random Access Memories']],
  ['Johann Sebastian Bach', ['Goldberg Variations (Gould 1981)']],
]) {
  for (const album of albums) {
    const n = album.startsWith('Goldberg') ? 32 : 11;
    for (let i = 1; i <= n; i++) library.rows.push(`${artist}/${album}/${String(i).padStart(2, '0')} - Трек ${i}.flac`);
  }
}
library.rows.push('Mr. Brown.mp3', 'single.wav');
library.rows.sort((a, b) => a.localeCompare(b, 'uk', { sensitivity: 'base' }));
Object.assign(state, { 'player.count': library.rows.length, 'player.index': 0 });

const dlnaTree = (name, kids) => ({ name, kids });
const dlnaTrack = (title, artist, playable = true) => ({ title, artist, playable });
const DLNA_SERVERS = [
  dlnaTree('NAS (MiniDLNA)', [
    dlnaTree('Музика', [
      dlnaTree('Виконавці', ['ДахаБраха', 'Океан Ельзи', 'Miles Davis', 'Daft Punk'].map((a) =>
        dlnaTree(a, Array.from({ length: a === 'Miles Davis' ? 30 : 5 }, (_, i) =>
          dlnaTrack(`${a} — пісня ${i + 1}`, a, i !== 3))))),
      dlnaTree('Альбоми', [dlnaTree('CD1', [dlnaTree('CD1', [dlnaTrack('Усередині CD1', 'Тест')])])]),
      dlnaTree('Порожня тека', []),
    ]),
    dlnaTree('Відео', []),
  ]),
  dlnaTree('Jellyfin', [dlnaTree('Music', [dlnaTrack('Blue in Green', 'Miles Davis'), dlnaTrack('So What', 'Miles Davis')])]),
];
const dlna = { revision: 1, busy: false, servers: true, found: [], path: [], offsets: [], status: 'Серверів: 0', timer: null };
function dlnaFolder() {
  let node = { kids: dlna.found };
  for (const i of dlna.path) node = node.kids[i];
  return node;
}
function dlnaView() {
  const off = dlna.servers ? 0 : dlna.offsets[dlna.offsets.length - 1];
  const all = dlna.servers ? dlna.found : dlnaFolder().kids;
  const rows = all.slice(off, off + 12).map((k) => k.kids
    ? { title: k.name, artist: '', folder: true, playable: false }
    : { title: k.title, artist: k.artist, folder: false, playable: k.playable });
  const title = dlna.servers ? 'Медіасервери' : dlnaFolder().name;
  return { revision: dlna.revision, busy: dlna.busy, servers: dlna.servers, can_back: !dlna.servers,
           count: dlna.busy && !dlna.status.startsWith('Пошук') ? 0 : rows.length,
           next: !dlna.servers && off + 12 < all.length, previous: !dlna.servers && off > 0,
           title, status: dlna.status, rows };
}
function dlnaWork(status, ms, then) {
  dlna.revision++;
  dlna.busy = true;
  dlna.status = status;
  clearTimeout(dlna.timer);
  dlna.timer = setTimeout(() => {
    then();
    dlna.busy = false;
    dlna.revision++;
    const n = dlnaView().count;
    dlna.status = dlna.servers ? `Серверів: ${n}` : `На сторінці: ${n}`;
  }, ms);
}
function dlnaCommand(code) {
  const cmd = code & 15, row = (code >> 4) & 255, rev = code >>> 12;
  if (cmd === 1) return dlnaWork('Пошук серверів…', 2500, () => { dlna.found = DLNA_SERVERS; dlna.servers = true; dlna.path = []; dlna.offsets = []; });
  if (cmd === 7) { clearTimeout(dlna.timer); dlna.busy = false; dlna.status = 'Скасовано'; return; }
  if (rev !== dlna.revision || dlna.busy) return;
  if (cmd === 3) {
    if (dlna.servers) return;
    if (dlna.path.length === 1) { dlna.revision++; dlna.servers = true; dlna.path = []; dlna.offsets = []; dlna.status = `Серверів: ${dlna.found.length}`; return; }
    return dlnaWork('Завантаження…', 400, () => { dlna.path.pop(); dlna.offsets.pop(); });
  }
  if (cmd === 4 || cmd === 5) {
    return dlnaWork('Завантаження…', 400, () => {
      const o = dlna.offsets.length - 1;
      dlna.offsets[o] = Math.max(0, dlna.offsets[o] + (cmd === 4 ? 12 : -12));
    });
  }
  const view = dlnaView();
  const r = view.rows[row];
  if (!r) return;
  const off = dlna.servers ? 0 : dlna.offsets[dlna.offsets.length - 1];
  if (r.folder) {
    return dlnaWork('Завантаження…', 500, () => { dlna.servers = false; dlna.path.push(off + row); dlna.offsets.push(0); });
  }
  if (!r.playable) { dlna.status = 'Формат не підтримується'; return; }
  dlna.status = `${cmd === 6 ? 'Далі' : 'Грає'}: ${r.title}`;
  if (cmd === 2) {
    const d = { 'audio.source': 5, 'radio.mode': 'push', 'dlna.state': 'PLAYING', 'dlna.title': r.title, 'dlna.artist': r.artist,
                'dlna.uri': 'http://nas/' + r.title, 'radio.url': 'http://nas/' + r.title, 'radio.pos_s': 0, 'radio.dur_s': 245,
                ...coverKeys((state['ui.cover'] || 0) + 8) };
    Object.assign(state, d);
    broadcast(d);
  }
}
// The Player's background for picture `id` (ui.cover_rgb): the colour of its top-left corner, by
// the rule of components/cover/src/cover_tone.cpp cover_backdrop() — that hue, lightness 20 %,
// saturation at most 45 %. 0 when there is no picture.
function coverRgb(id) {
  if (!id) return 0;
  const hue = (id * 47) % 360;
  const [r, g, b] = [127 + 127 * Math.cos(hue / 57.3), 200 * 0.1 + 30, 255 * 0.9].map((v) => v / 255);
  const mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn;
  const h = !d ? 0 : mx === r ? ((g - b) / d + 6) % 6 : mx === g ? (b - r) / d + 2 : (r - g) / d + 4;
  const sat = Math.min(0.45, d ? d / (1 - Math.abs(mx + mn - 1)) : 0);
  const c = 0.6 * sat, x = c * (1 - Math.abs((h % 2) - 1)), m = 0.2 - c / 2;
  const o = [[c, x, 0], [x, c, 0], [0, c, x], [0, x, c], [x, 0, c], [c, 0, x]][Math.floor(h) % 6];
  return o.reduce((v, ch) => (v << 8) | Math.round((ch + m) * 255), 0);
}
const coverKeys = (id) => ({ 'ui.cover': id, 'ui.cover_rgb': coverRgb(id) });

// a BMP of a cover, its colours from ui.cover: enough to see the thumbnail change with the track
function coverBmp(side) {
  const stride = (side * 3 + 3) & ~3;
  const buf = Buffer.alloc(54 + stride * side);
  buf.write('BM', 0);
  buf.writeUInt32LE(buf.length, 2);
  buf.writeUInt32LE(54, 10);
  buf.writeUInt32LE(40, 14);
  buf.writeInt32LE(side, 18);
  buf.writeInt32LE(-side, 22);
  buf.writeUInt16LE(1, 26);
  buf.writeUInt16LE(24, 28);
  buf.writeUInt32LE(stride * side, 34);
  const id = state['ui.cover'] || 0;
  const hue = (id * 47) % 360;
  for (let y = 0; y < side; y++) {
    for (let x = 0; x < side; x++) {
      const o = 54 + y * stride + x * 3;
      const ring = Math.hypot(x - side / 2, y - side / 2) < side / 5;
      const k = (x + y) / (2 * side);
      const [r, g, b] = ring ? [20, 20, 20] : [Math.round(127 + 127 * Math.cos((hue / 57.3))), Math.round(200 * k + 30), Math.round(255 * (1 - k))];
      buf[o] = b; buf[o + 1] = g; buf[o + 2] = r;
    }
  }
  return buf;
}

Object.assign(state, coverKeys(1));

function eqMoved() {
  eq.rev++;
  state['audio.eq_rev'] = eq.rev;
  setTimeout(() => broadcast({ 'audio.eq_rev': eq.rev }), 300);
}

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript',
  '.css': 'text/css',
  '.json': 'application/json',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
};

console.log('Starting rollup in watch mode...');
spawn('npx', ['rollup', '-c', '-w'], { stdio: 'inherit', shell: true, cwd: __dirname });

function json(res, code, body) {
  res.writeHead(code, { 'Content-Type': 'application/json' });
  res.end(typeof body === 'string' ? body : JSON.stringify(body));
}

// What a command does, roughly: enough to see the transport and the mini-player react.
function command(key, v) {
  const out = {};
  if (key === 'radio.cmd') out['radio.state'] = v === 2 ? 'idle' : 'playing';
  if (key === 'player.cmd') out['player.state'] = v === 2 || (v === 6 && state['player.state'] === 'playing') ? 'paused' : 'playing';
  if (key === 'player.cmd' && v === 1 && state['player.track']) {
    // a track asked for by its path, as the library page does: the card plays it
    const rel = state['player.track'].slice(library.root.length + 1);
    const i = library.rows.indexOf(rel);
    Object.assign(out, { 'audio.source': 4, 'player.index': Math.max(0, i), 'player.pos_s': 0, 'player.dur_s': 212,
                         'player.title': rel.split('/').pop().replace(/\.[^.]+$/, ''), 'player.artist': rel.split('/')[0],
                         'player.album': rel.split('/')[1] || '', 'player.format': 'FLAC 16/44.1',
                         ...coverKeys(i % 3 === 2 ? 0 : (state['ui.cover'] || 0) + 8) });
  }
  if (key === 'bt.cmd') out['bt.play_state'] = v === 2 || (v === 6 && state['bt.play_state'] === 'playing') ? 'paused' : 'playing';
  if (key === 'airplay.cmd') out['airplay.state'] = state['airplay.state'] === 'playing' ? 'paused' : 'playing';
  if (key === 'dlna.cmd') out['dlna.state'] = v === 3 || v === 2 ? 'PAUSED_PLAYBACK' : 'PLAYING';
  return out;
}

const server = createServer((req, res) => {
  const url = new URL(req.url, `http://localhost:${PORT}`);

  if (url.pathname === '/api/ui') {
    try {
      return json(res, 200, readFileSync(join(__dirname, '..', 'data', 'ui.json'), 'utf8'));
    } catch (e) {
      return json(res, 500, { error: 'data/ui.json not found (it is generated by the firmware build): ' + e.message });
    }
  }
  if (url.pathname === '/api/state') return json(res, 200, state);
  // The login as GET/POST /api/auth keep it (http_service.cpp, handle_post_auth). Off by default
  // so the dev pages open without signing in; the Access page turns it on to be looked at.
  if (url.pathname === '/api/auth' && req.method === 'GET') return json(res, 200, { enabled: auth.enabled, username: auth.username });
  if (url.pathname === '/api/auth' && req.method === 'POST') {
    let body = '';
    req.on('data', (c) => (body += c));
    req.on('end', () => {
      let e;
      try { e = JSON.parse(body); } catch { return json(res, 400, 'Invalid JSON'); }
      if (e.new_pass !== undefined) {
        if (e.current_pass !== auth.pass) return json(res, 403, { error: 'wrong_password' });
        if (String(e.new_pass).length < 4) return json(res, 400, 'Password too short (min 4)');
        auth.pass = e.new_pass;
      }
      if (e.username) auth.username = e.username;
      if (typeof e.enabled === 'boolean') auth.enabled = e.enabled;
      json(res, 200, { ok: true });
    });
    return;
  }
  if (url.pathname === '/api/settings' && req.method === 'POST') {
    let body = '';
    req.on('data', (c) => (body += c));
    req.on('end', () => {
      let delta = {};
      try {
        for (const [k, v] of Object.entries(JSON.parse(body))) {
          if (k === 'dlna.browse_cmd') dlnaCommand(v);
          else if (k.endsWith('.cmd')) Object.assign(delta, command(k, v));
          else { delta[k] = v; state[k] = v; }
        }
      } catch {
        return json(res, 400, 'Invalid JSON');
      }
      // off, the tone and AUX have no artwork: the Player's picture and its colour go (cover_now)
      if ([0, 1, 2].includes(delta['audio.source'])) Object.assign(delta, coverKeys(0));
      Object.assign(state, delta);
      broadcast(delta);
      if (delta['audio.port_meas'] === true) portMeasure();
      if (delta['audio.port_meas'] === false || (meter && delta['audio.source'] === 0)) portMeasureStop();
      json(res, 200, { ok: true });
    });
    return;
  }
  if (url.pathname === '/api/loglevel' && req.method === 'GET') return json(res, 200, log);
  if (url.pathname === '/api/loglevel' && req.method === 'POST') {
    let body = '';
    req.on('data', (c) => (body += c));
    req.on('end', () => {
      let e;
      try { e = JSON.parse(body); } catch { return json(res, 400, 'Invalid JSON'); }
      const tag = e.tag || '*';
      if (!LOG_LEVELS.includes(e.level) || LOG_LEVELS.indexOf(e.level) > LOG_LEVELS.indexOf(log.ceiling)) {
        return json(res, 400, 'level must be none|error|warn|info and not above the build ceiling');
      }
      if (tag === '*') { log.global = e.level; log.kept.forEach((k) => (k.level = 'info')); }
      else log.kept.forEach((k) => { if (k.tag === tag) k.level = e.level; });
      json(res, 200, { ok: true, tag, level: e.level });
    });
    return;
  }
  if (url.pathname === '/api/bindings' && req.method === 'GET') {
    return json(res, 200, readFileSync(join(__dirname, '..', 'boards', 'p4_player', 'bindings.json'), 'utf8'));
  }
  if (url.pathname === '/api/screenshot') {
    if (url.searchParams.has('screen')) return json(res, 400, 'the web must not switch the panel');
    setTimeout(() => {
      res.writeHead(200, { 'Content-Type': 'image/bmp', 'Cache-Control': 'no-store' });
      res.end(screenshotBmp());
    }, 700);
    return;
  }
  if (url.pathname === '/api/factory-reset' && req.method === 'POST') {
    json(res, 200, { ok: true, msg: 'Factory reset. Restarting...' });
    wifi.down = true;
    for (const ws of clients) ws.close();
    setTimeout(() => (wifi.down = false), 8000);
    return;
  }
  if (url.pathname === '/api/stations' && req.method === 'GET') return json(res, 200, stations);
  if (url.pathname === '/api/stations' && req.method === 'POST') {
    let body = '';
    req.on('data', (c) => (body += c));
    req.on('end', () => {
      let edit;
      try { edit = JSON.parse(body); } catch { return json(res, 400, { error: 'json' }); }
      if (!body.startsWith('{"rev":')) return json(res, 400, { error: 'rev_first' });
      if (edit.rev !== stations.rev) return json(res, 409, { error: 'changed', rev: stations.rev });
      if (!Array.isArray(edit.stations) || !edit.stations.length) return json(res, 400, { error: 'no_stations' });
      stations.list = { stations: edit.stations, presets: edit.presets };
      stations.rev += 2;
      const delta = { 'radio.list_rev': stations.rev, 'radio.count': edit.stations.length };
      Object.assign(state, delta);
      setTimeout(() => broadcast(delta), 300);   // the device applies it on its next tick
      json(res, 202, { ok: true });
    });
    return;
  }
  if (url.pathname === '/api/eq' && req.method === 'GET') {
    return json(res, 200, { rev: eq.rev, fs: 48000, preset: eq.preset, presets: offered(eq.preset), bands: eq.bands });
  }
  if (url.pathname === '/api/eq' && req.method === 'POST') {
    let body = '';
    req.on('data', (c) => (body += c));
    req.on('end', () => {
      let edit;
      try { edit = JSON.parse(body); } catch { return json(res, 400, { error: 'bad_edit' }); }
      if (edit.keep && !edit.bands && !edit.preset) return json(res, 200, { rev: eq.rev });
      if (edit.rev !== eq.rev) return json(res, 409, { error: 'changed', rev: eq.rev });
      if (edit.bands) {
        eq.bands = edit.bands;
        if (eq.preset !== 'custom') eq.preset = 'custom';
        eq.custom = eq.bands;
      } else if (edit.preset && edit.preset !== eq.preset) {
        if (eq.preset === 'custom') eq.custom = eq.bands;   // leaving "Свій" keeps it
        eq.preset = edit.preset;
        eq.bands = eqPreset(edit.preset);
      } else {
        return json(res, 200, { rev: eq.rev });
      }
      eqMoved();
      json(res, 200, { rev: eq.rev });
    });
    return;
  }
  if (url.pathname === '/api/time' && req.method === 'GET') {
    const d = new Date();
    return json(res, 200, { time: d.toTimeString().slice(0, 8), date: d.toLocaleDateString('uk-UA'), ntp_enabled: true,
                            timezone: 'EET-2EEST,M3.5.0/3,M10.5.0/4', synced: true, unix: Math.floor(d / 1000) });
  }
  if (url.pathname === '/api/ota' && req.method === 'GET') {
    return json(res, 200, { partition: 'ota_0', version: 'dev', idf: 'v5.5.5', date: 'Oct 11 2026', time: '22:26:18', board: 'modesp_p4_player' });
  }
  if (url.pathname === '/api/userdata' && req.method === 'GET') {
    return json(res, 200, { modesp_userdata: 1, files: { 'stations.json': JSON.stringify(stations.list), last_track: '/sdcard/a.mp3' } });
  }
  if (url.pathname === '/api/userdata' && req.method === 'POST') return json(res, 200, { ok: true, staged: 2 });
  if (url.pathname === '/api/backup' && req.method === 'GET') {
    return json(res, 200, { modesp_backup: 1, keys: { 'audio.volume': state['audio.volume'] ?? 30, 'ui.theme': state['ui.theme'] ?? 0 } });
  }
  if (url.pathname === '/dev/pair') {
    const p = bt.pairing;
    Object.assign(p, { seq: p.seq + 1, active: true, kind: Number(url.searchParams.get('kind') || 0), name: 'Galaxy S24',
                       code: 4096 + Math.floor(Math.random() * 900000), result: 255, by: 0, accepted: false });
    btPairKeys();
    return json(res, 200, { ok: true, seq: p.seq });
  }
  if (url.pathname === '/dev/pair_panel') {
    bt.pairing.by = 1;
    btPairKeys();
    btFinish(true);
    return json(res, 200, { ok: true });
  }
  if (url.pathname === '/api/bt' && req.method === 'GET') return json(res, 200, bt);
  if (url.pathname === '/api/bt' && req.method === 'POST') {
    let body = '';
    req.on('data', (c) => (body += c));
    req.on('end', () => {
      let e;
      try { e = JSON.parse(body); } catch { return json(res, 400, { error: 'bad_request' }); }
      if (e.pair) {
        const p = bt.pairing;
        if (!p.active || p.seq !== e.seq || p.by) return json(res, 409, { error: 'answered', by: p.by });
        p.by = 2; p.accepted = e.pair === 'accept';
        btPairKeys();
        btFinish(p.accepted);
        return json(res, 200, { ok: true });
      }
      for (const k of ['radio', 'discoverable', 'auto_reconnect']) if (k in e) bt[k] = !!e[k];
      if ('codecs' in e) bt.codecs_enabled = (e.codecs | 1) & bt.codecs_supported;
      if (e.name) bt.name = e.name;
      if (e.forget) bt.bonds = e.forget === 'all' ? [] : bt.bonds.filter((b) => b.addr !== e.forget);
      if (e.disconnect) bt.bonds.forEach((b) => (b.connected = false));
      bt.total = bt.bonds.length;
      btMoved();
      json(res, 200, { ok: true });
    });
    return;
  }
  if (url.pathname === '/dev/wifi') {
    wifi.scanFail = url.searchParams.get('scan') === 'fail';
    return json(res, 200, { ok: true, scanFail: wifi.scanFail });
  }
  if (url.pathname === '/api/wifi/scan') {
    // blocking, as on the device: seconds over the C6's RPC
    setTimeout(() => json(res, 200, wifi.scanFail ? { ok: false, networks: [] }
      : { ok: true, networks: wifi.nets.map(({ ssid, rssi, auth }) => ({ ssid, rssi: rssi - Math.floor(Math.random() * 4), auth })) }), 2500);
    return;
  }
  if (url.pathname === '/api/wifi/ap' && req.method === 'GET') {
    return json(res, 200, { ssid: wifi.ap.ssid, channel: wifi.ap.channel, password_set: wifi.ap.pass.length >= 8 });
  }
  if (url.pathname === '/api/wifi/ap' && req.method === 'POST') {
    let body = '';
    req.on('data', (c) => (body += c));
    req.on('end', () => {
      let e;
      try { e = JSON.parse(body); } catch { return json(res, 400, 'Invalid JSON'); }
      if (!e.ssid) return json(res, 400, 'Missing or empty ssid');
      const pass = e.password || '';
      if (pass && pass.length < 8) return json(res, 400, 'Password must be empty (open) or at least 8 chars');
      const ch = Number(e.channel);
      Object.assign(wifi.ap, { ssid: e.ssid, pass, channel: ch >= 1 && ch <= 13 ? ch : 1 });
      json(res, 200, { ok: true, msg: 'AP config saved. Restart to apply.' });
    });
    return;
  }
  if (url.pathname === '/api/wifi' && req.method === 'POST') {
    let body = '';
    req.on('data', (c) => (body += c));
    req.on('end', () => {
      let e;
      try { e = JSON.parse(body); } catch { return json(res, 400, 'Invalid JSON'); }
      if (e.forget) {
        setTimeout(() => wifiSet({ 'wifi.connected': false, 'wifi.mode': 'ap', 'wifi.ssid': wifi.ap.ssid, 'wifi.ip': '192.168.4.1', 'wifi.try': '' }), 300);
        return json(res, 200, { ok: true, forget: true });
      }
      if (e.join) {
        const len = (x) => Buffer.byteLength(x || '');
        if (!e.ssid || len(e.ssid) > 32 || len(e.password) > 64) return json(res, 400, 'ssid 1..32, password up to 64');
        if (state['wifi.try'] === 'connecting') return json(res, 409, { error: 'busy' });
        wifiJoin(e.ssid, e.password || '');
        return json(res, 200, { ok: true, join: true });
      }
      json(res, 200, { ok: true, msg: 'Credentials saved. Restart to connect.' });
    });
    return;
  }
  if (url.pathname === '/api/library') {
    const from = Math.max(0, Number(url.searchParams.get('from')) || 0);
    const n = Math.min(200, Math.max(1, Number(url.searchParams.get('n')) || 100));
    return setTimeout(() => json(res, 200, { gen: library.gen, busy: false, count: library.rows.length, cursor: state['player.index'] || 0,
                                             root: library.root, from, rows: library.rows.slice(from, from + n) }), 150);
  }
  if (url.pathname === '/api/dlna/browser') return json(res, 200, dlnaView());
  if (url.pathname === '/api/cover') {
    if (!state['ui.cover']) return json(res, 404, { error: 'no_cover' });
    const k = Math.min(10, Math.max(1, Math.floor(280 / (Number(url.searchParams.get('px')) || 140))));
    res.writeHead(200, { 'Content-Type': 'image/bmp', 'Cache-Control': 'private, no-cache', ETag: `"${state['ui.cover']}-${280 / k | 0}"` });
    return res.end(coverBmp(280 / k | 0));
  }
  if (url.pathname.startsWith('/api/')) return json(res, 200, { ok: true });

  const lang = url.pathname.match(/^\/lang\/(\w+)\.json$/);
  if (lang) {
    const p = join(__dirname, 'src', 'i18n', `${lang[1]}.json`);
    return existsSync(p) ? json(res, 200, readFileSync(p, 'utf8')) : json(res, 404, 'Not found');
  }

  // the generator's language packs for the generated pages (data/www/i18n, as on the device)
  const pack = url.pathname.match(/^\/i18n\/(\w+)\.json$/);
  if (pack) {
    const p = join(__dirname, '..', 'data', 'www', 'i18n', `${pack[1]}.json`);
    return existsSync(p) ? json(res, 200, readFileSync(p, 'utf8')) : json(res, 404, 'Not found');
  }

  // static: dist/ first, then public/, then what the device ships (radio_genres.json);
  // anything else is the page (the device does the same)
  const path = url.pathname === '/' ? '/index.html' : url.pathname;
  const shipped = path.endsWith('.json') ? [join(__dirname, '..', 'data', 'www', path)] : [];
  const file = [join(__dirname, 'dist', path), join(__dirname, 'public', path), ...shipped].find(existsSync)
    || join(__dirname, 'public', 'index.html');
  res.writeHead(200, { 'Content-Type': MIME[extname(file)] || 'application/octet-stream' });
  res.end(readFileSync(file));
});

const wss = new WebSocketServer({ server, path: '/ws' });
const clients = new Set();

function broadcast(delta) {
  const msg = JSON.stringify(delta);
  for (const ws of clients) if (ws.readyState === 1) ws.send(msg);
}

wss.on('connection', (ws) => {
  if (wifi.down) return ws.close();   // the panel is off the network, trying another one
  clients.add(ws);
  ws.send(JSON.stringify(state));
  ws.on('close', () => clients.delete(ws));
});

// Keep the clock and the track position moving (the panel sends what changed every 1.5 s).
setInterval(() => {
  const delta = {};
  for (const src of ['bt', 'player', 'airplay', 'radio']) {
    const playing = { bt: state['bt.play_state'], player: state['player.state'], airplay: state['airplay.state'], radio: state['radio.state'] }[src] === 'playing';
    const dur = state[`${src}.dur_s`] || 0;
    if (playing && dur) delta[`${src}.pos_s`] = (state[`${src}.pos_s`] + 1) % dur;
  }
  const now = new Date();
  delta['system.time'] = now.toTimeString().slice(0, 8);
  if (state['audio.source'] === 1 && (state['audio.tone_mode'] ?? 0) !== 0 && !meter) {
    toneMs += 1000;
    if (toneMs >= 60000) Object.assign(delta, { 'audio.tone_mode': 0, 'audio.source': 0 });
  } else {
    toneMs = 0;
  }
  Object.assign(state, delta);
  broadcast(delta);
  quiet({ 'system.uptime': (state['system.uptime'] || 0) + 1 });
}, 1000);
setInterval(() => {
  const j = (v, d, lo, hi) => Math.round(Math.min(hi, Math.max(lo, v + (Math.random() - 0.5) * d)) * 100) / 100;
  quiet({
    'audio.cpu_pct': j(state['audio.cpu_pct'] ?? 30, 6, 5, 95),
    'audio.dsp_pct': j(state['audio.dsp_pct'] ?? 19, 4, 2, 60),
    'audio.dsp_max_pct': j(state['audio.dsp_max_pct'] ?? 23, 6, 5, 90),
    'audio.lim_gr': Math.random() < 0.3 ? j(1.2, 2, 0, 4) : 0,
    'system.main_loop_max_ms': Math.round(j(state['system.main_loop_max_ms'] ?? 12, 8, 10, 60)),
    'system.heap_internal': Math.round(j(state['system.heap_internal'] ?? 120000, 4000, 40000, 200000)),
    'ui.touch_x': Math.floor(Math.random() * 480),
    'ui.touch_y': Math.floor(Math.random() * 800),
  });
}, 2000);

server.listen(PORT, () => console.log(`\n  P4 Player web UI dev server: http://localhost:${PORT}\n`));
