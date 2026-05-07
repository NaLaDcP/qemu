'use strict';

const http = require('http');
const fs   = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');
const koffi = require('koffi');

// ── Pin names (optional — mounted at /config/pins.json) ──────────────────────
const pinNames = {};
try {
  Object.assign(pinNames, JSON.parse(fs.readFileSync('/config/pins.json', 'utf8')));
  console.log('Loaded pin names from /config/pins.json');
} catch (_) {}

// ── Constants ─────────────────────────────────────────────────────────────────
const MAGIC          = 0xABCD;
const MSG_TYPE_PIN   = 0;
const MSG_TYPE_REG   = 1;
const MSG_TYPE_QUERY = 2;
const PORT           = 8080;
const MSG_SIZE       = 12;
const MQ_BUF         = 8192;
const RETRY_MS       = 3000;
const POLL_MS        = 20;    // poll /from_qemu every 20ms
const O_RDONLY       = 0;
const O_WRONLY       = 1;
const O_NONBLOCK     = 0x800;

const REG_NAMES = { 0: 'dir', 4: 'odr', 8: 'dat', 12: 'ier', 16: 'imr', 20: 'icr' };

// ── Koffi FFI ─────────────────────────────────────────────────────────────────
let lib;
for (const name of ['librt.so.1', 'libc.so.6']) {
  try { lib = koffi.load(name); break; } catch (_) {}
}
if (!lib) { console.error('Cannot load librt/libc'); process.exit(1); }

const mq_open    = lib.func('int mq_open(const char *name, int oflag)');
const mq_close   = lib.func('int mq_close(int mqdes)');
const mq_send    = lib.func('int mq_send(int mqdes, const uint8_t *msg, size_t len, unsigned int prio)');
const mq_receive = lib.func('long mq_receive(int mqdes, uint8_t *buf, size_t len, void *prio)');

// ── Message encoding ──────────────────────────────────────────────────────────
function packMsg(type, pin, state) {
  const buf = Buffer.alloc(MSG_SIZE);
  buf.writeUInt16LE(MAGIC, 0);
  buf.writeUInt8(type,     2);
  buf.writeUInt8(0,        3);
  buf.writeUInt32LE(pin,   4);
  buf.writeUInt32LE(state, 8);
  return buf;
}

function unpackMsg(buf) {
  if (buf.length < MSG_SIZE)         return null;
  if (buf.readUInt16LE(0) !== MAGIC) return null;
  return {
    type:  buf.readUInt8(2),
    pin:   buf.readUInt32LE(4),
    state: buf.readUInt32LE(8),
  };
}

// ── State ─────────────────────────────────────────────────────────────────────
let mqFromFd      = -1;
let mqToFd        = -1;
let pollTimer     = null;
let qemuConnected = false;
const regState    = {};
const clients     = new Set();

// ── WebSocket broadcast ───────────────────────────────────────────────────────
function broadcast(msg) {
  const s = JSON.stringify(msg);
  for (const ws of clients) {
    if (ws.readyState === 1) ws.send(s);
  }
}

function setConnected(val) {
  if (qemuConnected === val) return;
  qemuConnected = val;
  broadcast({ type: 'qemu_status', connected: val });
  console.log(val ? 'QEMU connected' : 'QEMU disconnected');
}

function closeMqueues() {
  if (pollTimer)  { clearInterval(pollTimer); pollTimer = null; }
  if (mqFromFd >= 0) { mq_close(mqFromFd); mqFromFd = -1; }
  if (mqToFd   >= 0) { mq_close(mqToFd);   mqToFd   = -1; }
}

process.on('exit', closeMqueues);

// ── Poll loop — reads all pending messages from /from_qemu ────────────────────
const pollBuf = Buffer.alloc(MQ_BUF);

function handleMsg(msg) {
  if (msg.type === MSG_TYPE_PIN) {
    broadcast({ type: 'pin_update', pin: msg.pin, state: msg.state });
  } else if (msg.type === MSG_TYPE_REG) {
    const offset = msg.pin;
    regState[offset] = msg.state;
    const name = REG_NAMES[offset] ?? `reg_${offset}`;
    broadcast({ type: 'reg_update', offset, name, value: msg.state });
  }
}

function poll() {
  while (true) {
    const n = mq_receive(mqFromFd, pollBuf, pollBuf.length, null);
    if (n < 0) break;          // EAGAIN — queue empty, stop for now
    if (n < MSG_SIZE) continue;
    const msg = unpackMsg(pollBuf);
    if (msg) handleMsg(msg);
  }
}

// ── Connection with auto-retry ────────────────────────────────────────────────
function scheduleRetry() {
  setTimeout(tryConnect, RETRY_MS);
}

function tryConnect() {
  closeMqueues();
  const fromFd = mq_open('/from_qemu', O_RDONLY | O_NONBLOCK);
  const toFd   = mq_open('/to_qemu',   O_WRONLY);
  if (fromFd < 0 || toFd < 0) {
    if (fromFd >= 0) mq_close(fromFd);
    if (toFd   >= 0) mq_close(toFd);
    scheduleRetry();
    return;
  }
  mqFromFd = fromFd;
  mqToFd   = toFd;
  setConnected(true);
  mq_send(mqToFd, packMsg(MSG_TYPE_QUERY, 0, 0), MSG_SIZE, 0);
  pollTimer = setInterval(poll, POLL_MS);
}

// ── HTTP ──────────────────────────────────────────────────────────────────────
const server = http.createServer((req, res) => {
  if (req.url === '/' || req.url === '/index.html') {
    fs.readFile(path.join(__dirname, 'gpio_ui.html'), (err, data) => {
      if (err) { res.writeHead(500); res.end('Cannot read gpio_ui.html'); return; }
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      res.end(data);
    });
  } else {
    res.writeHead(404); res.end('Not found');
  }
});

// ── WebSocket ─────────────────────────────────────────────────────────────────
const wss = new WebSocketServer({ server });

wss.on('connection', (ws) => {
  clients.add(ws);
  ws.send(JSON.stringify({ type: 'pin_names', names: pinNames }));
  ws.send(JSON.stringify({ type: 'qemu_status', connected: qemuConnected }));
  for (const [offset, value] of Object.entries(regState)) {
    const o = Number(offset);
    const name = REG_NAMES[o] ?? `reg_${o}`;
    ws.send(JSON.stringify({ type: 'reg_update', offset: o, name, value }));
  }
  ws.on('message', (data) => {
    if (!qemuConnected) {
      ws.send(JSON.stringify({ type: 'error', msg: 'QEMU not connected' }));
      return;
    }
    try {
      const { pin, state } = JSON.parse(data);
      mq_send(mqToFd, packMsg(MSG_TYPE_PIN, Number(pin), Number(state)), MSG_SIZE, 0);
    } catch (e) {
      ws.send(JSON.stringify({ type: 'error', msg: e.message }));
    }
  });
  ws.on('close', () => clients.delete(ws));
});

server.listen(PORT, () => {
  console.log(`GPIO web interface at http://localhost:${PORT}`);
  tryConnect();
});
