'use strict';

const { test } = require('node:test');
const assert = require('node:assert');

const { Connection, read } = require('..');

const CONNINFO =
  process.env.NP_CONNINFO || 'host=/tmp dbname=postgres user=postgres';

test('zero-copy views alias the segment; copy:true detaches', () => {
  const conn = new Connection(CONNINFO);
  conn.exec(
    `SELECT native_query_shm('zc_seg', ` +
      `'SELECT g::int4 AS i4 FROM generate_series(1, 10) g')`
  );

  const zc = read('zc_seg');
  const cp = read('zc_seg', { copy: true });

  // the zero-copy view spans the whole payload; the copy is exactly one column
  assert.ok(zc.columns[0].value.buffer.byteLength > 10 * 4);
  assert.strictEqual(cp.columns[0].value.buffer.byteLength, 10 * 4);

  assert.deepStrictEqual([...zc.columns[0].value], [...cp.columns[0].value]);
  assert.strictEqual(zc.columns[0].value[0], 1);
  assert.strictEqual(zc.columns[0].value[9], 10);

  conn.close();
});
