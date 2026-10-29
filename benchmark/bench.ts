/*
 * bench.ts — the same data across three transports, read and write:
 *
 *   tcp    libpq over 127.0.0.1        (text rows -> JS objects)
 *   unix   libpq over the unix socket  (text rows -> JS objects)
 *   shm    native columnar over /dev/shm (typed arrays, zero-copy, ring control)
 *
 * Usage: node --import tsx ../benchmark/bench.ts [nrows ...]
 *        (default 100000 1000000 10000000)
 */
import type { ConnectionLike, RingLike } from '../node-extension';
import { Connection, read, Ring, synthCopy, synthSegment } from '../node-extension';

const QUERY =
  'SELECT id, account_id, balance, active, name, email, score, age FROM bench';
const SEG = 'npseg';
const RING = 'np_ring';

const CONN_TCP =
  process.env.NP_TCP || 'host=127.0.0.1 dbname=postgres user=postgres';
const CONN_UNIX =
  process.env.NP_UNIX || 'host=/tmp dbname=postgres user=postgres';

const now = (): number => Number(process.hrtime.bigint()) / 1e6;
const rate = (n: number, ms: number): number => n / (ms / 1000) / 1e6; // M rows/s

function fmt(n: number): string {
  if (n >= 1000000) return `${Math.floor(n / 1000000)}M`;
  if (n >= 1000) return `${Math.floor(n / 1000)}K`;
  return `${n}`;
}

function setupTable(conn: ConnectionLike, n: number): void {
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

/** Read rows over libpq; the cost is libpq's text decode plus the JS objects. */
function readText(conn: ConnectionLike): number {
  const t0 = now();
  const rows = conn.exec(QUERY);
  let bytes = 0;
  for (const r of rows) bytes += (r.name ?? '').length;
  const ms = now() - t0;
  if (bytes === 0) throw new Error('read: empty result');
  return ms;
}

const READ_BATCH = 100000;

/** Pull chunks through the ring (no socket) and read each from /dev/shm. */
function readShm(ring: RingLike): number {
  const t0 = now();
  ring.open(SEG, QUERY);
  for (;;) {
    const n = ring.next(READ_BATCH);
    if (n === 0) break;
    read(SEG); // zero-copy chunk, consumed before the next fetch
  }
  ring.close();
  return now() - t0;
}

/* ---- WRITE --------------------------------------------------------------- */

function writeCopy(conn: ConnectionLike, n: number): number {
  conn.exec('DROP TABLE IF EXISTS dst; CREATE TABLE dst (LIKE bench)');
  const t0 = now();
  conn.copyFrom('COPY dst FROM STDIN', synthCopy(n)); // native build + send
  return now() - t0;
}

function writeShm(conn: ConnectionLike, ring: RingLike, n: number): number {
  conn.exec('DROP TABLE IF EXISTS dst; CREATE TABLE dst (LIKE bench)');
  const t0 = now();
  synthSegment(SEG, n); // native: generate rows straight into /dev/shm
  ring.write(SEG, 'dst'); // no socket
  return now() - t0;
}

/* ---- output -------------------------------------------------------------- */

const col = (s: unknown, w: number, right: boolean): string =>
  right ? String(s).padStart(w) : String(s).padEnd(w);

function header(title: string): void {
  console.log(
    `${title}: ` +
      ['tcp', 'unix', 'shm', 'shm/tcp', 'shm/unix']
        .map((s, i) => col(s, i < 3 ? 8 : 10, false))
        .join(' ') +
      ' '
  );
}

function row(
  load: number,
  a: number,
  b: number,
  c: number,
  ra: number,
  rb: number
): void {
  console.log(
    '  ' +
      col(fmt(load), 8, false) +
      ' ' +
      col(a.toFixed(2), 8, true) +
      'M' +
      ' ' +
      col(b.toFixed(2), 8, true) +
      'M' +
      ' ' +
      col(c.toFixed(2), 8, true) +
      'M' +
      ' ' +
      col(ra.toFixed(2), 8, true) +
      'x' +
      ' ' +
      col(rb.toFixed(2), 9, true) +
      'x'
  );
}

function main(): void {
  let sizes = process.argv
    .slice(2)
    .map(Number)
    .filter((x) => x > 0);
  if (sizes.length === 0) sizes = [100000, 1000000, 10000000];

  const tcp = new Connection(CONN_TCP);
  const unix = new Connection(CONN_UNIX);
  tcp.exec('SET client_min_messages = warning'); // keep the tables clean
  unix.exec('SET client_min_messages = warning');

  // One SQL call bootstraps the worker; the shm path then uses only the ring.
  const ring = new Ring(RING);
  ring.start(unix);

  header('READ (rows/s)');
  for (const n of sizes) {
    setupTable(unix, n);
    const rt = readText(tcp);
    const ru = readText(unix);
    const rs = readShm(ring);
    row(n, rate(n, rt), rate(n, ru), rate(n, rs), rt / rs, ru / rs);
  }

  console.log();
  header('WRITE (rows/s)');
  for (const n of sizes) {
    const wt = writeCopy(tcp, n);
    const wu = writeCopy(unix, n);
    const ws = writeShm(unix, ring, n);
    row(n, rate(n, wt), rate(n, wu), rate(n, ws), wt / ws, wu / ws);
  }

  ring.stop();
  tcp.close();
  unix.close();
}

main();
