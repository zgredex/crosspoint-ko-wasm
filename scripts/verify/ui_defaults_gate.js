#!/usr/bin/env node
// Static contract for the shipped Korean UI and its clean/reset defaults.

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..', '..');
const html = fs.readFileSync(path.join(root, 'web', 'index.html'), 'utf8');
const app = fs.readFileSync(path.join(root, 'web', 'app.js'), 'utf8');
let failures = 0;

function check(ok, label) {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

function tagFor(id) {
  const match = html.match(new RegExp(`<[^>]+id=["']${id}["'][^>]*>`, 'i'));
  return match ? match[0] : '';
}

function optionFor(selectId, value) {
  const select = html.match(new RegExp(`<select[^>]+id=["']${selectId}["'][\\s\\S]*?<\\/select>`, 'i'));
  if (!select) return '';
  const escaped = String(value).replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const option = select[0].match(new RegExp(`<option[^>]+value=["']${escaped}["'][^>]*>`, 'i'));
  return option ? option[0] : '';
}

check(!/BOOK SETUP|CROSSPOINT-KO RENDERING|placeholder=["']e\.g\.|title=["']MIN,MAX/.test(html),
      'visible interface prose is Korean');
check(/<span>리더 화면 여백<\/span>/.test(html), 'Korean-fork screen-margin label is exact');
const margin = tagFor('screenMargin');
check(/min=["']5["']/.test(margin) && /max=["']40["']/.test(margin) &&
      /step=["']5["']/.test(margin) && /value=["']5["']/.test(margin),
      'screen margin is 5..40 step 5 with default 5');
check(/selected/i.test(optionFor('deviceProfile', 'x4')), 'X4 is the clean-install device default');
check(/selected/i.test(optionFor('orientation', '0')), 'portrait is the clean-install orientation default');
check(/selected/i.test(optionFor('imageDither', '2')), 'blue-noise image dithering is the default');
const xtch = html.match(/<input[^>]+name=["']exportModeRadio["'][^>]+value=["']1["'][^>]*>/i);
check(!!xtch && /checked/i.test(xtch[0]), '2-bit XTCH is the default output mode');
check(/checked/i.test(tagFor('lz4Wrap')), 'LZ4/XTCZ wrapping is enabled by default');

const defaults = (app.match(/function applyDefaults\(\) \{[\s\S]*?\n  \}/) || [''])[0];
check(/els\.deviceProfile\.value = 'x4'/.test(defaults) &&
      /els\.orientation\.value = '0'/.test(defaults), 'reset restores X4 portrait');
check(/state\.mode = 1/.test(defaults) && /els\.lz4Wrap\.checked = true/.test(defaults) &&
      /els\.imageDither\.value = '2'/.test(defaults),
      'reset restores 2-bit XTCH + LZ4 + blue noise');

if (failures) {
  console.error(`ui-defaults gate FAILED (${failures})`);
  process.exit(1);
}
console.log('ui-defaults gate OK');
