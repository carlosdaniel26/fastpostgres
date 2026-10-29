'use strict';

const { test } = require('node:test');
const assert = require('node:assert');

const { Connection, read } = require('..');

const CONNINFO =
  process.env.NP_CONNINFO || 'host=/tmp dbname=postgres user=postgres';

test('streams batches through a cursor and dispatches handlers', () => {
  const conn = new Connection(CONNINFO);
  conn.exec('DROP TABLE IF EXISTS stream_src');
  conn.exec(`
    CREATE TABLE stream_src AS
    SELECT g::int4 AS i4, ('x' || g)::text AS t
    FROM generate_series(1, 5000) g
  `);

  const batches = [];
  const seenColumns = [];
  let seen = 0;

  conn.exec('BEGIN');
  conn.exec(`SELECT native_stream_open('stream_seg', 'SELECT * FROM stream_src')`);
  for (;;) {
    const rows = conn.exec(`SELECT native_stream_next('stream_seg', 2000)`);
    const n = Number(rows[0].native_stream_next);
    batches.push(n);
    if (n === 0) break;

    const chunk = read('stream_seg', {
      handlers: {
        onKind: {
          5: (field) => seenColumns.push(field.name),
        },
      },
    });
    assert.strictEqual(chunk.rowCount, n);
    seen += chunk.rowCount;
  }
  conn.exec(`SELECT native_stream_close('stream_seg')`);
  conn.exec('COMMIT');

  assert.deepStrictEqual(batches, [2000, 2000, 1000, 0]);
  assert.strictEqual(seen, 5000);
  assert.ok(seenColumns.includes('t')); // the text handler fired per chunk

  conn.close();
});
