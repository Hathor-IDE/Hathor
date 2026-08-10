#!/usr/bin/env node
/**
 * strudel-lsp-server.cjs — CommonJS bootstrap for the Strudel LSP server.
 *
 * Patches the @kabelsalat/web ESM exports before importing the ESM
 * strudel-lsp-server.js module (which uses @strudel/mini / @strudel/core).
 *
 * This is a build-time/runtime shim: the @kabelsalat/web v0.4.1 package
 * lacks an `exports` field in its package.json, causing ESM import resolution
 * to fail for the `SalatRepl` named export that @strudel/core requires. We
 * add the exports field before the dynamic import so the ESM entry point
 * (dist/index.mjs) is resolved correctly.
 */

const { existsSync, readFileSync, writeFileSync } = require('fs');
const { join } = require('path');

const kabelsalatPkgPath = join(__dirname, 'node_modules', '@kabelsalat', 'web', 'package.json');
if (existsSync(kabelsalatPkgPath)) {
    const pkg = JSON.parse(readFileSync(kabelsalatPkgPath, 'utf8'));
    if (!pkg.exports) {
        pkg.exports = { '.': './dist/index.mjs' };
        writeFileSync(kabelsalatPkgPath, JSON.stringify(pkg, null, 2));
    }
}

// Now dynamically import the ESM module
import('./strudel-lsp-server.js').catch(err => {
    console.error('[strudel-lsp-server] Fatal:', err);
    process.exit(1);
});
