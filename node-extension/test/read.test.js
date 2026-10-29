'use strict';

const { test } = require('node:test');
const assert = require('node:assert');

const { Connection, read } = require('..');

const CONNINFO =
  process.env.NP_CONNINFO || 'host=/tmp dbname=postgres user=postgres';

function textAt(col, i) {
  const { bytes, offsets } = col;
  return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength)
    .subarray(offsets[i], offsets[i + 1])
    .toString('utf8');
}

test('read() decodes a segment into aligned typed arrays', () => {
  const conn = new Connection(CONNINFO);
  conn.exec(`
    SELECT native_query_shm('read_seg', $q$
      SELECT g::int4 AS i4, g::int8 AS i8, g::float8 AS f8,
             (g % 2 = 0) AS flag, ('user' || g)::text AS name
      FROM generate_series(1, 100) g
    $q$)
  `);

  const res = read('read_seg');
  assert.strictEqual(res.rowCount, 100);
  assert.deepStrictEqual(
    res.columns.map((c) => c.name),
    ['i4', 'i8', 'f8', 'flag', 'name']
  );

  const [i4, i8, f8, flag, name] = res.columns.map((c) => c.value);
  assert.ok(i4 instanceof Int32Array);
  assert.ok(i8 instanceof BigInt64Array);
  assert.ok(f8 instanceof Float64Array);
  assert.ok(flag instanceof Uint8Array);

  assert.strictEqual(i4[0], 1);
  assert.strictEqual(i4[99], 100);
  assert.strictEqual(i8[99], 100n);
  assert.strictEqual(f8[0], 1);
  assert.strictEqual(flag[0], 0); // g=1 is odd
  assert.strictEqual(flag[1], 1); // g=2 is even
  assert.strictEqual(textAt(name, 0), 'user1');
  assert.strictEqual(textAt(name, 99), 'user100');

  conn.close();
});

test('read() handles every width (alignment check)', () => {
  const conn = new Connection(CONNINFO);
  conn.exec(`
    SELECT native_query_shm('types_seg', $q$
      SELECT g::int2 AS i2, g::int4 AS i4, g::int8 AS i8, g::float4 AS f4,
             g::float8 AS f8, g::numeric(10,2) AS num, (g % 2 = 0) AS b,
             ('c' || g)::char(4) AS ch
      FROM generate_series(1, 8) g
    $q$)
  `);

  const { columns } = read('types_seg');
  // each numeric type is copied verbatim at its own width (no widening)
  assert.ok(columns[0].value instanceof Int16Array); // int2
  assert.ok(columns[1].value instanceof Int32Array); // int4
  assert.ok(columns[2].value instanceof BigInt64Array); // int8
  assert.ok(columns[3].value instanceof Float32Array); // float4
  assert.ok(columns[4].value instanceof Float64Array); // float8
  assert.ok(columns[5].value instanceof Float64Array); // numeric -> float8
  assert.ok(columns[6].value instanceof Uint8Array); // bool
  assert.ok(columns[7].value.bytes instanceof Uint8Array); // char -> text
  assert.strictEqual(columns[0].value[7], 8);
  assert.strictEqual(columns[2].value[7], 8n);
  assert.strictEqual(columns[3].value[7], 8);
  assert.strictEqual(textAt(columns[7].value, 0).trim(), 'c1');

  conn.close();
});
