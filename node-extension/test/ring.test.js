'use strict';

const { test } = require('node:test');
const assert = require('node:assert');

const { Connection, read, Ring, synthSegment } = require('..');

const CONNINFO =
  process.env.NP_CONNINFO || 'host=/tmp dbname=postgres user=postgres';
const SEG = 'ring_seg';
const RING = 'np_ring_test';

test('ring: stream and ingest without a socket in the loop', () => {
  const conn = new Connection(CONNINFO);
  conn.exec('DROP TABLE IF EXISTS ring_src');
  conn.exec(`
    CREATE TABLE ring_src AS
    SELECT g::int4 AS i4, ('x' || g)::text AS t
    FROM generate_series(1, 2500) g
  `);

  const ring = new Ring(RING);
  ring.start(conn); // the one SQL bootstrap call

  const batches = [];
  let seen = 0;
  ring.open(SEG, 'SELECT * FROM ring_src');
  for (;;) {
    const n = ring.next(1000);
    batches.push(n);
    if (n === 0) break;
    const chunk = read(SEG);
    assert.strictEqual(chunk.rowCount, n);
    seen += n;
  }
  ring.close();

  assert.deepStrictEqual(batches, [1000, 1000, 500, 0]);
  assert.strictEqual(seen, 2500);

  // write path through the ring (synthSegment produces the 8 bench columns)
  conn.exec(`
    DROP TABLE IF EXISTS ring_dst;
    CREATE TABLE ring_dst (id int4, account_id int8, balance float8,
                           active bool, name text, email text,
                           score float8, age int4)
  `);
  synthSegment(SEG, 3000);
  assert.strictEqual(ring.write(SEG, 'ring_dst'), 3000);

  const rows = conn.exec('SELECT count(*) AS c FROM ring_dst');
  assert.strictEqual(Number(rows[0].c), 3000);

  ring.stop();
  conn.close();
});

test('ring: push mode yields a JS object per row as it is produced', () => {
  const conn = new Connection(CONNINFO);
  const ring = new Ring('np_ring_push');
  ring.start(conn);

  let count = 0;
  const first = [];
  ring.pushOpen(
    "SELECT g::int4 AS i4, ('x' || g)::text AS t, (g*1.5)::float8 AS f " +
      'FROM generate_series(1, 2000) g'
  );
  ring.pushRows((row) => {
    if (first.length < 3) first.push(row);
    count++;
  });

  assert.strictEqual(count, 2000);
  assert.deepStrictEqual(first[0], { i4: 1, t: 'x1', f: 1.5 });
  assert.strictEqual(first[2].i4, 3);

  ring.stop();
  conn.close();
});
