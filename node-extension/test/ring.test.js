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
