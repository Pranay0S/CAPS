'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');

const P = require('../lib/protocol');
const { Session } = require('../lib/session');

test('parses telemetry into typed fields', () => {
  const r = P.parseLine('TLM,3100,RUNNING,NONE,100.00,160.130,99.9,30.02,10.0,0.08,0.08,0.00,0.0861,ROC_FLOW|ROC_PRESS,SPIKE');
  assert.equal(r.type, 'TLM');
  assert.equal(r.tick_ms, 3100);
  assert.equal(r.state, 'RUNNING');
  assert.equal(r.flow_meas, 160.13);
  assert.equal(r.anomaly, 'ROC_FLOW|ROC_PRESS');
  assert.equal(r.attack, 'SPIKE');
});

test('parses log, ack, err and header records', () => {
  assert.deepEqual(P.parseLine('LOG,0,INFO,SIM,boot: period=100ms seed=1'), {
    type: 'LOG', tick_ms: 0, level: 'INFO', module: 'SIM', message: 'boot: period=100ms seed=1',
  });
  assert.equal(P.parseLine('ACK,10,rate,state=IDLE').module, 'rate');
  assert.equal(P.parseLine('ERR,10,rate,value out of range').level, 'WARN');
  assert.deepEqual(P.parseLine('#TLM,tick_ms,state').columns, ['tick_ms', 'state']);
  assert.equal(P.parseLine('   '), null);
  assert.equal(P.parseLine('garbage').type, 'RAW');
});

test('strips trailing CR from Windows pipes', () => {
  assert.equal(P.parseLine('ACK,1,start,ok\r').message, 'ok');
});

test('CSV fields are quoted only when needed', () => {
  assert.equal(P.csvField('plain'), 'plain');
  assert.equal(P.csvField('a,b'), '"a,b"');
  assert.equal(P.csvField('say "hi"'), '"say ""hi"""');
  assert.equal(P.csvField(null), '');
  assert.equal(P.csvRow([1, 'x', undefined]), '1,x,\r\n');
});

test('combined rows have the same column count for every record type', () => {
  const cols = P.combinedColumns().length;
  const tlm = { ...P.parseLine('TLM,100,IDLE,NONE,1,2,3,4,5,6,7,8,9,NONE,NONE'), host_time: 't' };
  const log = { ...P.parseLine('LOG,100,WARN,MON,anomaly SEQ'), host_time: 't' };
  for (const rec of [tlm, log]) {
    const row = P.combinedRow(rec).trimEnd().split(',');
    assert.equal(row.length, cols, rec.type);
  }
});

test('telemetry CSV holds only pump-generated columns; ground truth goes to labels', () => {
  const rec = P.parseLine(
    'TLM,3100,RUNNING,NONE,100.00,160.130,99.9,30.02,10.0,0.08,0.08,0.00,0.0861,ROC_FLOW,SPIKE,31,1,31,1,NONE');
  assert.equal(rec.flow_seq, 31);
  assert.equal(rec.fault, 'NONE');

  const tcols = P.telemetryColumns();
  assert.deepEqual(tcols, P.PUMP_COLUMNS);
  for (const leaked of ['attack', 'fault', 'anomaly', 'flow_true', 'pressure_true', 'volume_true', 'host_time']) {
    assert.ok(!tcols.includes(leaked), `${leaked} must not be in telemetry`);
  }
  assert.equal(P.telemetryRow(rec), '3100,RUNNING,NONE,100,160.13,31,1,30.02,31,1,0.08,0,0.0861\r\n');
  assert.equal(P.labelRow(rec), '3100,SPIKE,NONE,ROC_FLOW,99.9,10,0.08\r\n');
});

test('older simulators without the new columns still export', () => {
  const old = P.DEFAULT_TLM_COLUMNS.slice(0, 14);
  assert.ok(!P.telemetryColumns(old).includes('flow_seq'));
  assert.ok(!P.labelColumns(old).includes('fault'));
});

test('session writes telemetry, labels and event CSV files and exports from memory', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ivpump-'));
  const s = new Session(dir, { seed: 1 });
  s.add(P.parseLine('LOG,0,INFO,SIM,boot'));
  s.add(P.parseLine('TLM,100,IDLE,NONE,100,0,0,8,8,0,0,0,0,NONE,NONE,1,1,1,1,NONE'));
  s.add({ type: 'CMD', tick_ms: 100, level: 'INFO', module: 'cli', message: 'attack spike' });
  s.close();
  await new Promise((r) => setTimeout(r, 50));

  const tlm = fs.readFileSync(s.telemetryPath, 'utf8').trim().split('\r\n');
  assert.equal(tlm[0], P.PUMP_COLUMNS.join(','));
  assert.equal(tlm.length, 2);

  const lbl = fs.readFileSync(s.labelsPath, 'utf8').trim().split('\r\n');
  assert.equal(lbl[0], P.LABEL_COLUMNS.join(','));
  assert.equal(lbl[1], '100,NONE,NONE,NONE,0,8,0');

  const evt = fs.readFileSync(s.eventsPath, 'utf8').trim().split('\r\n');
  assert.equal(evt[0], P.EVENT_COLUMNS.join(','));
  assert.equal(evt.length, 3);
  assert.match(evt[2], /,CMD,INFO,cli,attack spike$/);

  assert.deepEqual([...s.csv('labels')].join('').trim().split('\r\n'), lbl);
  const all = [...s.csv('all')].join('').trim().split('\r\n');
  assert.equal(all.length, 4);
  assert.match(all[1], /,LOG,/);
  assert.match(all[2], /,TLM,100,IDLE,/);
  fs.rmSync(dir, { recursive: true, force: true });
});
