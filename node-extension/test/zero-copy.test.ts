import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Connection, read } from '../lib';

const CONNINFO =
  process.env.NP_CONNINFO || 'host=/tmp dbname=postgres user=postgres';

test('zero-copy views alias the segment; copy:true detaches', () => {
  const conn = new Connection(CONNINFO);
  conn.exec(
    `SELECT native_query_shm('zc_seg', ` +
      `'SELECT g::int4 AS i4 FROM generate_series(1, 10) g')`
  );

  const zc = read('zc_seg').columns[0].value as Int32Array;
  const cp = read('zc_seg', { copy: true }).columns[0].value as Int32Array;

  // the zero-copy view spans the whole payload; the copy is exactly one column
  assert.ok(zc.buffer.byteLength > 10 * 4);
  assert.strictEqual(cp.buffer.byteLength, 10 * 4);

  assert.deepStrictEqual([...zc], [...cp]);
  assert.strictEqual(zc[0], 1);
  assert.strictEqual(zc[9], 10);

  conn.close();
});
