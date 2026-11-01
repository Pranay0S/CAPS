#!/usr/bin/env node
'use strict';
// Standalone attack console - the "secondary terminal".
//
// Connects to a running testbench server and sends commands to the simulator
// exactly as the GUI terminal does. Responses and warnings stream back live.
//
//   node inject.js [--host 127.0.0.1] [--port 8534]       interactive
//   node inject.js attack spike 80 10                      one-shot

const http = require('http');
const readline = require('readline');

const args = process.argv.slice(2);
let host = '127.0.0.1';
let port = 8534;
const rest = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--host') host = args[++i];
  else if (args[i] === '--port') port = Number(args[++i]);
  else rest.push(args[i]);
}
const oneShot = rest.length ? rest.join(' ') : null;

const tty = process.stdout.isTTY;
const c = (code, s) => (tty ? `\x1b[${code}m${s}\x1b[0m` : s);
const red = (s) => c('31', s);
const green = (s) => c('32', s);
const yellow = (s) => c('33', s);
const dim = (s) => c('2', s);
const bold = (s) => c('1', s);

const HELP = `
${bold('Attacks')} (sensor-bus adversary)
  attack spike   [mag=60]  [samples=6]   transient on flow & pressure
  attack drift   [rate=0.4] [samples]    slowly growing bias, ml/h per sample
  attack replay  [window=50] [samples]   loop recorded frames (old seq numbers)
  attack freeze  [-] [samples]           hold last reading constant
  attack dropout [-] [samples]           swallow frames (jamming)
  attack noise   [sigma=25] [samples]    heavy Gaussian noise
  attack tamper  <rate>                  overwrite setpoint in firmware RAM
  attack clear                           stop the active attack
${bold('Faults')}
  fault occlusion on|off                 physical downstream occlusion
${bold('Pump console')}
  start | pause | stop | clear | status | rate <ml/h> | vtbi <ml>
  ids on|off | loglevel debug|info|warn|error
${bold('Local')}
  .status   last telemetry sample     .watch   toggle live telemetry line
  .help     this text                 .quit    exit (also Ctrl+D)
`;

function post(pathname, body) {
  return new Promise((resolve, reject) => {
    const data = JSON.stringify(body);
    const req = http.request(
      { host, port, path: pathname, method: 'POST', headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data) } },
      (res) => {
        let text = '';
        res.on('data', (d) => (text += d));
        res.on('end', () => {
          try {
            resolve({ status: res.statusCode, body: JSON.parse(text) });
          } catch {
            resolve({ status: res.statusCode, body: { error: text } });
          }
        });
      },
    );
    req.on('error', reject);
    req.end(data);
  });
}

async function send(line) {
  try {
    const r = await post('/api/cmd', { line, source: 'cli' });
    if (!r.body.ok) print(red(`✗ ${r.body.error}`));
    return r.body.ok;
  } catch (e) {
    print(red(`✗ cannot reach server at ${host}:${port} (${e.code || e.message})`));
    return false;
  }
}

// ---- live stream -----------------------------------------------------------

let rl = null;
let latest = null;
let watch = false;
let pendingAck = null; // resolves when the response to our command arrives

function print(s) {
  if (rl) {
    readline.clearLine(process.stdout, 0);
    readline.cursorTo(process.stdout, 0);
    console.log(s);
    rl.prompt(true);
  } else {
    console.log(s);
  }
}

function fmtTlm(t) {
  return `${(t.tick_ms / 1000).toFixed(1).padStart(7)}s  ${t.state.padEnd(8)} ` +
    `flow ${t.flow_meas.toFixed(1).padStart(6)} (true ${t.flow_true.toFixed(1).padStart(6)}) ml/h  ` +
    `P ${t.pressure_meas.toFixed(1).padStart(5)} kPa  alarm=${t.alarm} attack=${t.attack} anomaly=${t.anomaly}`;
}

function onEvent(e) {
  if (e.type === 'ACK' || e.type === 'ERR') {
    const msg = `${e.type === 'ACK' ? green('✓') : red('✗')} ${e.module}: ${e.message}`;
    print(msg);
    pendingAck?.();
    pendingAck = null;
  } else if (e.type === 'CMD') {
    if (e.module !== 'cli') print(dim(`  [${e.module}] ${e.message}`));
  } else if (e.type === 'LOG' || e.type === 'SYS') {
    const t = e.tick_ms != null ? `${(e.tick_ms / 1000).toFixed(1)}s` : '';
    const line = `${t.padStart(7)} ${e.level.padEnd(5)} ${e.module}: ${e.message}`;
    if (e.level === 'ERROR') print(red(line));
    else if (e.level === 'WARN') print(yellow(line));
    else if (e.module === 'SIM' || e.module === 'SERVER') print(dim(line));
  }
}

function connectStream(onReady) {
  const req = http.get({ host, port, path: '/events' }, (res) => {
    res.setEncoding('utf8');
    let buf = '';
    res.on('data', (chunk) => {
      buf += chunk;
      let idx;
      while ((idx = buf.indexOf('\n\n')) >= 0) {
        const block = buf.slice(0, idx);
        buf = buf.slice(idx + 2);
        let event = 'message';
        let data = '';
        for (const l of block.split('\n')) {
          if (l.startsWith('event: ')) event = l.slice(7);
          else if (l.startsWith('data: ')) data += l.slice(6);
        }
        if (!data) continue;
        const obj = JSON.parse(data);
        if (event === 'hello') {
          latest = obj.latest;
          onReady(obj);
        } else if (event === 'tlm') {
          latest = obj;
          if (watch) print(dim(fmtTlm(obj)));
        } else if (event === 'event') {
          onEvent(obj);
        }
      }
    });
    res.on('end', () => {
      print(red('✗ server closed the stream'));
      process.exit(1);
    });
  });
  req.on('error', (e) => {
    console.error(red(`✗ cannot reach server at ${host}:${port} (${e.code || e.message})`));
    console.error('  start it with:  node gui/server.js');
    process.exit(1);
  });
}

// ---- main ------------------------------------------------------------------

if (oneShot) {
  connectStream(async () => {
    const done = new Promise((r) => (pendingAck = r));
    if (await send(oneShot)) await Promise.race([done, new Promise((r) => setTimeout(r, 2000))]);
    process.exit(0);
  });
} else {
  connectStream((hello) => {
    console.log(bold('CAPS IV pump - attack console'));
    console.log(dim(`connected to ${host}:${port}  session ${hello.session?.id ?? '-'}  sim ${hello.running ? 'running' : 'stopped'}`));
    console.log(dim('type .help for commands'));

    rl = readline.createInterface({ input: process.stdin, output: process.stdout, prompt: red('inject> '), historySize: 200 });
    rl.prompt();
    rl.on('line', async (raw) => {
      const line = raw.trim();
      if (!line) return rl.prompt();
      if (line === '.quit' || line === '.exit') return rl.close();
      if (line === '.help' || line === 'help') print(HELP);
      else if (line === '.status') print(latest ? fmtTlm(latest) : dim('no telemetry yet'));
      else if (line === '.watch') {
        watch = !watch;
        print(dim(`live telemetry ${watch ? 'on' : 'off'}`));
      } else await send(line);
      rl.prompt();
    });
    rl.on('close', () => process.exit(0));
  });
}
