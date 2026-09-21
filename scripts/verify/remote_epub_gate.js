#!/usr/bin/env node
// Static trust-boundary contract for the convenience ?epub= loader. Runtime
// behavior is additionally exercised in the browser pass.

const fs = require('fs');
const path = require('path');

const app = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'app.js'), 'utf8');
let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

check(/const MAX_REMOTE_EPUB_BYTES = 512 \* 1024 \* 1024;/.test(app),
      'remote EPUB byte ceiling is explicit');
check(/const url = new URL\(rawUrl, location\.href\);[\s\S]*?url\.origin !== location\.origin/.test(app),
      'remote loader rejects cross-origin URLs before fetch');
check(/redirect: 'error'/.test(app) && /finalUrl\.origin !== location\.origin/.test(app),
      'redirects cannot bypass the same-origin policy');
check(/if \(!response\.ok\)/.test(app), 'remote loader rejects non-2xx HTTP responses');
check(/headers\.get\('content-length'\)/.test(app), 'declared response size is checked before body materialization');
check(/TransformStream[\s\S]*?received > maxBytes - bytes/.test(app),
      'streamed response bytes are capped while downloading');
check((app.match(/loadRemoteEpub\(auto\)/g) || []).length === 2,
      'boot and worker-recovery paths share the hardened loader');
check(!/fetch\(auto\)/.test(app), 'no direct unbounded ?epub= fetch remains');

if (failures) process.exit(1);
console.log('remote-epub gate OK');
