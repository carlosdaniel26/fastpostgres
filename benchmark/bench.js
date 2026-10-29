'use strict';

/*
 * bench.js — the same data across three transports, read and write:
 *
 *   tcp    libpq over 127.0.0.1       (text rows -> JS objects)
 *   unix   libpq over the unix socket (text rows -> JS objects)
 *   shm    native columnar over /dev/shm (typed arrays, zero-copy)
 *
 * Usage: node benchmark/bench.js [nrows ...]   (default 100000 1000000 10000000)
 */
const {
  Connection,
  read,
  synthSegment,
  synthCopy,
} = require('../node-extension');

const QUERY =
  'SELECT id, account_id, balance, active, name, email, score, age FROM bench';
const SEG = 'npseg';

const CONN_TCP =
  process.env.NP_TCP || 'host=127.0.0.1 dbname=postgres user=postgres';
const CONN_UNIX =
  process.env.NP_UNIX || 'host=/tmp dbname=postgres user=postgres';

const now = () => Number(process.hrtime.bigint()) / 1e6;
const rate = (n, ms) => n / (ms / 1000) / 1e6; // M rows/s

function fmt(n) {
  if (n >= 1000000) return `${Math.floor(n / 1000000)}M`;
  if (n >= 1000) return `${Math.floor(n / 1000)}K`;
  return `${n}`;
}

function setupTable(conn, n) {
  conn.exec('DROP TABLE IF EXISTS bench');
  conn.exec(
    `CREATE TABLE bench AS
     SELECT g::int4 AS id, (g::int8 * 1000) AS account_id,
            (g*1.5)::float8 AS balance, (g%2=0)::bool AS active,
            ('user'||g)::text AS name,
            ('user'||g||'@example.com')::text AS email,
            (g*0.25)::float8 AS score, (g%100)::int4 AS age
     FROM generate_series(1, ${n}) g`
  );
}

/* ---- READ ---------------------------------------------------------------- */

function readText(conn) {
  const t0 = now();
  const rows = conn.exec(QUERY);
  let sum = 0;
  for (const r of rows) sum += r.name.length;
  const ms = now() - t0;
  if (sum < 0) throw new Error('unreachable');
  return ms;
}

const READ_BATCH = 100000;

/* Batched: pull chunks through a cursor instead of materializing the result. */
function readShm(conn) {
  const t0 = now();
  conn.exec('BEGIN');
  conn.exec(`SELECT native_stream_open('${SEG}', $q$${QUERY}$q$)`);
  for (;;) {
    const rows = conn.exec(`SELECT native_stream_next('${SEG}', ${READ_BATCH})`);
    if (Number(rows[0].native_stream_next) === 0) break;
    read(SEG); // zero-copy chunk, consumed before the next fetch
  }
  conn.exec(`SELECT native_stream_close('${SEG}')`);
  conn.exec('COMMIT');
  return now() - t0;
}

/* ---- WRITE --------------------------------------------------------------- */

function writeCopy(conn, n) {
  conn.exec('DROP TABLE IF EXISTS dst; CREATE TABLE dst (LIKE bench)');
  const t0 = now();
  conn.copyFrom('COPY dst FROM STDIN', synthCopy(n)); // native build + send
  return now() - t0;
}

function writeShm(conn, n) {
  conn.exec('DROP TABLE IF EXISTS dst; CREATE TABLE dst (LIKE bench)');
  const t0 = now();
  synthSegment(SEG, n); // native: generate rows straight into /dev/shm
  conn.exec(`SELECT native_write_shm('${SEG}', 'dst')`);
  return now() - t0;
}

/* ---- output -------------------------------------------------------------- */

const col = (s, w, right) => (right ? String(s).padStart(w) : String(s).padEnd(w));

function header(title) {
  console.log(
    `${title}: ` + ['tcp', 'unix', 'shm', 'shm/tcp', 'shm/unix']
      .map((s, i) => col(s, i < 3 ? 8 : 10, false))
      .join(' ') + ' '
  );
}

function row(load, a, b, c, ra, rb) {
  console.log(
    '  ' +
      col(fmt(load), 8, false) +
      ' ' +
      col(a.toFixed(2), 8, true) + 'M' +
      ' ' +
      col(b.toFixed(2), 8, true) + 'M' +
      ' ' +
      col(c.toFixed(2), 8, true) + 'M' +
      ' ' +
      col(ra.toFixed(2), 8, true) + 'x' +
      ' ' +
      col(rb.toFixed(2), 9, true) + 'x'
  );
}

function main() {
  let sizes = process.argv.slice(2).map(Number).filter((x) => x > 0);
  if (sizes.length === 0) sizes = [100000, 1000000, 10000000];

  const tcp = new Connection(CONN_TCP);
  const unix = new Connection(CONN_UNIX);
  tcp.exec('SET client_min_messages = warning');  // keep the tables clean
  unix.exec('SET client_min_messages = warning');

  header('READ (rows/s)');
  for (const n of sizes) {
    setupTable(unix, n);
    const rt = readText(tcp);
    const ru = readText(unix);
    const rs = readShm(unix);
    row(n, rate(n, rt), rate(n, ru), rate(n, rs), rt / rs, ru / rs);
  }

  console.log();
  header('WRITE (rows/s)');
  for (const n of sizes) {
    const wt = writeCopy(tcp, n);
    const wu = writeCopy(unix, n);
    const ws = writeShm(unix, n);
    row(n, rate(n, wt), rate(n, wu), rate(n, ws), wt / ws, wu / ws);
  }

  tcp.close();
  unix.close();
}

main();
