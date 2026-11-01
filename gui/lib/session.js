'use strict';
// A recording session: every record received from (or sent to) one simulator
// run. Kept in memory for export and also appended to CSV files on disk as it
// arrives, so nothing is lost if the server is killed.

const fs = require('fs');
const path = require('path');
const P = require('./protocol');

const MAX_RECORDS = 1_000_000;

function stamp(d = new Date()) {
  const pad = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}_` +
    `${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`;
}

class Session {
  constructor(logDir, meta = {}) {
    this.id = stamp();
    this.meta = meta;
    this.startedAt = new Date().toISOString();
    this.tlmColumns = P.DEFAULT_TLM_COLUMNS;
    this.records = [];
    this.truncated = false;

    fs.mkdirSync(logDir, { recursive: true });
    this.telemetryPath = path.join(logDir, `session_${this.id}_telemetry.csv`);
    this.eventsPath = path.join(logDir, `session_${this.id}_events.csv`);
    this.tlmOut = null; // opened lazily so a late #TLM header can set columns
    this.evtOut = fs.createWriteStream(this.eventsPath);
    this.evtOut.write(P.csvRow(P.EVENT_COLUMNS));
  }

  setColumns(columns) {
    if (!this.tlmOut) this.tlmColumns = columns;
  }

  /** Stamp, store and persist a record. Returns the stamped record. */
  add(rec) {
    rec.host_time = rec.host_time || new Date().toISOString();
    if (this.records.length < MAX_RECORDS) this.records.push(rec);
    else this.truncated = true;

    if (rec.type === 'TLM') {
      if (!this.tlmOut) {
        this.tlmOut = fs.createWriteStream(this.telemetryPath);
        this.tlmOut.write(P.csvRow(P.telemetryColumns(this.tlmColumns)));
      }
      this.tlmOut.write(P.telemetryRow(rec, this.tlmColumns));
    } else {
      this.evtOut.write(P.eventRow(rec));
    }
    return rec;
  }

  recent(type, n) {
    const out = [];
    for (let i = this.records.length - 1; i >= 0 && out.length < n; i--) {
      const r = this.records[i];
      if (type === 'TLM' ? r.type === 'TLM' : r.type !== 'TLM') out.push(r);
    }
    return out.reverse();
  }

  /** Yields CSV chunks for kind = telemetry | events | all. */
  *csv(kind) {
    const cols = this.tlmColumns;
    if (kind === 'telemetry') {
      yield P.csvRow(P.telemetryColumns(cols));
      for (const r of this.records) if (r.type === 'TLM') yield P.telemetryRow(r, cols);
    } else if (kind === 'events') {
      yield P.csvRow(P.EVENT_COLUMNS);
      for (const r of this.records) if (r.type !== 'TLM') yield P.eventRow(r);
    } else {
      yield P.csvRow(P.combinedColumns(cols));
      for (const r of this.records) yield P.combinedRow(r, cols);
    }
  }

  close() {
    this.tlmOut?.end();
    this.evtOut.end();
  }

  info() {
    return {
      id: this.id,
      startedAt: this.startedAt,
      records: this.records.length,
      truncated: this.truncated,
      files: [this.telemetryPath, this.eventsPath].map((p) => path.basename(p)),
      ...this.meta,
    };
  }
}

module.exports = { Session };
