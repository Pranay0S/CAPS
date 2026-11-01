#!/usr/bin/env node
'use strict';
// CAPS IV pump testbench server.
//
// Spawns the C firmware simulator, parses its log stream, records every
// record to CSV, and serves the browser GUI. Commands from the GUI and from
// the standalone attack console (inject.js) are forwarded to the simulator's
// stdin.
//
//   node server.js [--port 8534] [--bin path/to/ivpump_sim] [--period-ms 100]
//                  [--seed N] [--logs ../logs]

const http = require('http');
const fs = require('fs');
const path = require('path');
const readline = require('readline');
const { spawn } = require('child_process');

const { parseLine } = require('./lib/protocol');
const { Session } = require('./lib/session');

// ---- configuration ---------------------------------------------------------

function parseArgs(argv) {
  const exe = process.platform === 'win32' ? 'ivpump_sim.exe' : 'ivpump_sim';
  const opts = {
    port: Number(process.env.PORT) || 8534,
    host: '127.0.0.1',
    bin: path.resolve(__dirname, '..', 'firmware', 'build', exe),
    periodMs: 100,
    seed: null,
    logs: path.resolve(__dirname, '..', 'logs'),
  };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    const next = () => argv[++i];
    if (a === '--port') opts.port = Number(next());
    else if (a === '--host') opts.host = next();
    else if (a === '--bin') opts.bin = path.resolve(next());
    else if (a === '--period-ms') opts.periodMs = Number(next());
    else if (a === '--seed') opts.seed = Number(next());
    else if (a === '--logs') opts.logs = path.resolve(next());
    else {
      console.error(`unknown option ${a}`);
      process.exit(2);
    }
  }
  return opts;
}

const opts = parseArgs(process.argv);
const PUBLIC_DIR = path.join(__dirname, 'public');
const MAX_CMD_LEN = 120;

// ---- live clients (Server-Sent Events) ----------------------------------------

const clients = new Set();

function broadcast(event, data) {
  const payload = `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
  for (const res of clients) res.write(payload);
}

// ---- simulator process ----------------------------------------------------------

let sim = null;      // ChildProcess
let session = null;  // Session
let latest = null;   // last TLM record

function record(rec) {
  session.add(rec);
  if (rec.type === 'TLM') latest = rec;
  broadcast(rec.type === 'TLM' ? 'tlm' : 'event', rec);
}

function systemEvent(level, message) {
  record({ type: 'SYS', tick_ms: latest?.tick_ms ?? null, level, module: 'SERVER', message });
}

function startSim({ seed = opts.seed, periodMs = opts.periodMs } = {}) {
  if (!fs.existsSync(opts.bin)) {
    throw new Error(`simulator binary not found: ${opts.bin}\nBuild it first: cmake -S firmware -B firmware/build -G Ninja && cmake --build firmware/build`);
  }
  const actualSeed = Number.isFinite(seed) ? seed : Math.floor(Math.random() * 2 ** 31);
  session?.close();
  session = new Session(opts.logs, { seed: actualSeed, periodMs });
  latest = null;

  const args = ['--header', '--period-ms', String(periodMs), '--seed', String(actualSeed)];
  const child = spawn(opts.bin, args, { stdio: ['pipe', 'pipe', 'pipe'] });
  sim = child;
  broadcast('session', session.info());
  systemEvent('INFO', `simulator started (pid ${child.pid}) seed=${actualSeed} period=${periodMs}ms`);

  readline.createInterface({ input: child.stdout }).on('line', (line) => {
    const rec = parseLine(line, session.tlmColumns);
    if (!rec) return;
    if (rec.type === 'HDR') session.setColumns(rec.columns);
    else record(rec);
  });
  readline.createInterface({ input: child.stderr }).on('line', (line) => {
    systemEvent('WARN', `sim stderr: ${line}`);
  });
  child.stdin.on('error', () => {}); // EPIPE if the sim died; reported via 'exit'
  child.on('exit', (code, signal) => {
    if (sim === child) sim = null;
    systemEvent(code === 0 ? 'INFO' : 'ERROR', `simulator exited (code=${code} signal=${signal})`);
    broadcast('status', status());
  });
  broadcast('status', status());
}

function stopSim() {
  return new Promise((resolve) => {
    if (!sim) return resolve();
    const child = sim;
    const timer = setTimeout(() => child.kill(), 1000);
    child.once('exit', () => {
      clearTimeout(timer);
      resolve();
    });
    child.stdin.end('quit\n');
  });
}

function sendCommand(line, source) {
  if (!sim) return { ok: false, error: 'simulator is not running' };
  record({ type: 'CMD', tick_ms: latest?.tick_ms ?? null, level: 'INFO', module: source, message: line });
  sim.stdin.write(line + '\n');
  return { ok: true };
}

function status() {
  return { running: !!sim, session: session?.info() ?? null, latest };
}

// ---- HTTP ----------------------------------------------------------------------

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.svg': 'image/svg+xml',
};

function sendJson(res, code, obj) {
  res.writeHead(code, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(obj));
}

function readBody(req, limit = 4096) {
  return new Promise((resolve, reject) => {
    let body = '';
    req.on('data', (chunk) => {
      body += chunk;
      if (body.length > limit) {
        reject(new Error('body too large'));
        req.destroy();
      }
    });
    req.on('end', () => resolve(body));
    req.on('error', reject);
  });
}

function serveStatic(req, res, urlPath) {
  const rel = urlPath === '/' ? 'index.html' : urlPath.slice(1);
  const file = path.resolve(PUBLIC_DIR, rel);
  if (!file.startsWith(PUBLIC_DIR + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    res.writeHead(404);
    return res.end('not found');
  }
  res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
}

function serveCsv(res, kind) {
  if (!session) return sendJson(res, 404, { error: 'no session' });
  const name = `ivpump_${session.id}_${kind}.csv`;
  res.writeHead(200, {
    'Content-Type': 'text/csv; charset=utf-8',
    'Content-Disposition': `attachment; filename="${name}"`,
  });
  for (const chunk of session.csv(kind)) res.write(chunk);
  res.end();
}

function openEventStream(req, res) {
  res.writeHead(200, {
    'Content-Type': 'text/event-stream',
    'Cache-Control': 'no-cache',
    Connection: 'keep-alive',
  });
  res.write(`event: hello\ndata: ${JSON.stringify({
    ...status(),
    history: session ? session.recent('TLM', 1200) : [],
    events: session ? session.recent('EVENT', 300) : [],
  })}\n\n`);
  clients.add(res);
  req.on('close', () => clients.delete(res));
}

async function handle(req, res) {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
  const route = `${req.method} ${url.pathname}`;

  if (route === 'GET /events') return openEventStream(req, res);
  if (route === 'GET /api/status') return sendJson(res, 200, status());

  if (route === 'POST /api/cmd') {
    let body;
    try {
      body = JSON.parse(await readBody(req));
    } catch {
      return sendJson(res, 400, { ok: false, error: 'expected JSON {"line": "..."}' });
    }
    const line = typeof body.line === 'string' ? body.line.trim() : '';
    const source = body.source === 'cli' ? 'cli' : 'gui';
    if (!line || line.length > MAX_CMD_LEN || /[\r\n\0]/.test(line)) {
      return sendJson(res, 400, { ok: false, error: `command must be one line, 1-${MAX_CMD_LEN} chars` });
    }
    const result = sendCommand(line, source);
    return sendJson(res, result.ok ? 200 : 409, result);
  }

  if (route === 'POST /api/restart') {
    let body = {};
    try {
      body = JSON.parse((await readBody(req)) || '{}');
    } catch { /* defaults */ }
    await stopSim();
    try {
      startSim({
        seed: body.seed === undefined || body.seed === '' ? opts.seed : Number(body.seed),
        periodMs: Number(body.periodMs) > 0 ? Number(body.periodMs) : opts.periodMs,
      });
    } catch (e) {
      return sendJson(res, 500, { ok: false, error: e.message });
    }
    return sendJson(res, 200, { ok: true, ...status() });
  }

  const csv = url.pathname.match(/^\/api\/export\/(telemetry|events|all)\.csv$/);
  if (req.method === 'GET' && csv) return serveCsv(res, csv[1]);

  if (req.method === 'GET') return serveStatic(req, res, url.pathname);

  res.writeHead(405);
  res.end();
}

const server = http.createServer((req, res) => {
  handle(req, res).catch((e) => {
    console.error(e);
    if (!res.headersSent) sendJson(res, 500, { error: e.message });
    else res.end();
  });
});

// Keep-alive comments so proxies/browsers don't drop idle SSE connections.
setInterval(() => {
  for (const res of clients) res.write(': ping\n\n');
}, 15000).unref();

try {
  startSim();
} catch (e) {
  console.error(e.message);
  process.exit(1);
}

server.listen(opts.port, opts.host, () => {
  console.log(`IV pump testbench:   http://${opts.host}:${opts.port}`);
  console.log(`attack console:      node ${path.relative(process.cwd(), path.join(__dirname, 'inject.js'))} --port ${opts.port}`);
  console.log(`session CSVs in:     ${opts.logs}`);
});

async function shutdown() {
  await stopSim();
  session?.close();
  process.exit(0);
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
