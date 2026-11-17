'use strict';
// Parsing of the ivpump_sim line protocol and CSV export helpers.
//
//   #TLM,<col>,<col>,...                  column header (sent with --header)
//   TLM,<tick_ms>,<state>,...             telemetry, one per control period
//   LOG,<tick_ms>,<LEVEL>,<MODULE>,<msg>  firmware / harness event
//   ACK,<tick_ms>,<verb>,<msg>            command accepted
//   ERR,<tick_ms>,<verb>,<msg>            command rejected

const DEFAULT_TLM_COLUMNS = [
  'tick_ms', 'state', 'alarm', 'target_rate', 'flow_meas', 'flow_true',
  'pressure_meas', 'pressure_true', 'volume_est', 'volume_true', 'vtbi',
  'duty', 'anomaly', 'attack', 'flow_seq', 'flow_ok', 'pressure_seq',
  'pressure_ok', 'fault',
];

const TEXT_TLM_FIELDS = new Set(['state', 'alarm', 'anomaly', 'attack', 'fault']);

// The TLM stream mixes two kinds of data. They are written to separate CSVs so
// the telemetry file is exactly what the pump itself could log on the device.
//
// PUMP_COLUMNS: produced or observed by the firmware (sensor frames as
// received, actuator command, state machine). Model inputs come only from here.
const PUMP_COLUMNS = [
  'tick_ms', 'state', 'alarm', 'target_rate', 'flow_meas', 'flow_seq', 'flow_ok',
  'pressure_meas', 'pressure_seq', 'pressure_ok', 'volume_est', 'vtbi', 'duty',
];
// LABEL_COLUMNS: harness ground truth (attack, fault, plant state the pump
// cannot see) plus the rule-based monitor's flags as a baseline detector.
// Training/evaluation targets only, never model inputs.
const LABEL_COLUMNS = [
  'tick_ms', 'attack', 'fault', 'anomaly', 'flow_true', 'pressure_true', 'volume_true',
];

const EVENT_COLUMNS = ['host_time', 'tick_ms', 'type', 'level', 'module', 'message'];

function toNumber(s) {
  const n = Number(s);
  return Number.isFinite(n) ? n : null;
}

/** Parse one line from the simulator. Returns a record object, or null for blank lines. */
function parseLine(line, tlmColumns = DEFAULT_TLM_COLUMNS) {
  const text = line.replace(/\r$/, '');
  if (!text.trim()) return null;

  if (text.startsWith('#TLM,')) {
    return { type: 'HDR', columns: text.slice(5).split(',') };
  }

  const parts = text.split(',');
  const kind = parts[0];

  if (kind === 'TLM') {
    const rec = { type: 'TLM' };
    tlmColumns.forEach((col, i) => {
      const raw = parts[i + 1] ?? '';
      rec[col] = TEXT_TLM_FIELDS.has(col) ? raw : toNumber(raw);
    });
    return rec;
  }

  if (kind === 'LOG' && parts.length >= 5) {
    return {
      type: 'LOG',
      tick_ms: toNumber(parts[1]),
      level: parts[2],
      module: parts[3],
      message: parts.slice(4).join(','),
    };
  }

  if ((kind === 'ACK' || kind === 'ERR') && parts.length >= 4) {
    return {
      type: kind,
      tick_ms: toNumber(parts[1]),
      level: kind === 'ACK' ? 'INFO' : 'WARN',
      module: parts[2],
      message: parts.slice(3).join(','),
    };
  }

  return { type: 'RAW', level: 'DEBUG', module: 'SIM', message: text, tick_ms: null };
}

function csvField(v) {
  if (v === null || v === undefined) return '';
  const s = String(v);
  return /[",\r\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
}

function csvRow(values) {
  return values.map(csvField).join(',') + '\r\n';
}

/** Pump-only columns present in this stream, in PUMP_COLUMNS order. */
function telemetryColumns(tlmColumns = DEFAULT_TLM_COLUMNS) {
  return PUMP_COLUMNS.filter((c) => tlmColumns.includes(c));
}

function telemetryRow(rec, tlmColumns = DEFAULT_TLM_COLUMNS) {
  return csvRow(telemetryColumns(tlmColumns).map((c) => rec[c]));
}

/** Ground-truth label columns present in this stream. */
function labelColumns(tlmColumns = DEFAULT_TLM_COLUMNS) {
  return LABEL_COLUMNS.filter((c) => tlmColumns.includes(c));
}

function labelRow(rec, tlmColumns = DEFAULT_TLM_COLUMNS) {
  return csvRow(labelColumns(tlmColumns).map((c) => rec[c]));
}

function eventRow(rec) {
  return csvRow(EVENT_COLUMNS.map((c) => rec[c]));
}

/** One CSV holding every record type, ordered as received. This is a debug
 *  dump: it includes ground truth, so don't train on it. */
function combinedColumns(tlmColumns = DEFAULT_TLM_COLUMNS) {
  return ['host_time', 'record_type', ...tlmColumns, 'level', 'module', 'message'];
}

function combinedRow(rec, tlmColumns = DEFAULT_TLM_COLUMNS) {
  if (rec.type === 'TLM') {
    return csvRow([rec.host_time, 'TLM', ...tlmColumns.map((c) => rec[c]), '', '', '']);
  }
  const tlmPart = tlmColumns.map((c) => (c === 'tick_ms' ? rec.tick_ms : ''));
  return csvRow([rec.host_time, rec.type, ...tlmPart, rec.level, rec.module, rec.message]);
}

module.exports = {
  DEFAULT_TLM_COLUMNS,
  PUMP_COLUMNS,
  LABEL_COLUMNS,
  EVENT_COLUMNS,
  parseLine,
  csvField,
  csvRow,
  telemetryColumns,
  telemetryRow,
  labelColumns,
  labelRow,
  eventRow,
  combinedColumns,
  combinedRow,
};
