/**
 * Node client for fastpostgres.
 *
 * `read()` is zero-copy by default: the typed arrays alias the mapped
 * `/dev/shm` segment, so they are only valid until that segment is rewritten —
 * i.e. until the next chunk comes through the ring. Use `{ copy: true }` to
 * detach.
 */
import { existsSync } from 'node:fs';
import { join } from 'node:path';

import type { NativeModule } from './types';

/* Resolve the addon built by node-gyp, from source (lib/) or from dist/lib/. */
function addonPath(): string {
  const bases = [
    join(__dirname, '..', 'build'), // lib/  -> <pkg>/build
    join(__dirname, '..', '..', 'build'), // dist/lib/ -> <pkg>/build
  ];
  for (const base of bases) {
    for (const config of ['Release', 'Debug']) {
      const candidate = join(base, config, 'fastpostgres.node');
      if (existsSync(candidate)) return candidate;
    }
  }
  throw new Error(
    'fastpostgres: native addon is not built — run `npm run build` (needs node-gyp)'
  );
}

// eslint-disable-next-line @typescript-eslint/no-var-requires
const native = require(addonPath()) as NativeModule;

export const Connection = native.Connection;
export const Ring = native.Ring;
export const read = native.read;
export const writeSegment = native.writeSegment;
export const synthSegment = native.synthSegment;
export const synthCopy = native.synthCopy;

export * from './types';
