'use strict';

// The native addon built by node-gyp (binding.gyp -> build/<config>).
const { existsSync } = require('node:fs');
const { join } = require('node:path');

const native = ['Release', 'Debug']
  .map((config) => join(__dirname, '..', 'build', config, 'nativepostgress.node'))
  .find((path) => existsSync(path));

if (!native) {
  throw new Error(
    'nativepostgress: native addon is not built — run `npm run build` (needs node-gyp)'
  );
}

module.exports = require(native);
