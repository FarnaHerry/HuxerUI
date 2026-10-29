"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const source = fs.readFileSync(process.argv[2], "utf8");
function embedded(name, parameters) {
  const declaration = source.indexOf(", " + name + ",");
  assert(declaration >= 0, name);
  const start = source.indexOf(", {", declaration) + 3;
  const end = source.indexOf("\n});", start);
  assert(end > start, name);
  return new Function(...parameters, source.slice(start, end));
}
const prepare = embedded("PrepareWebFont", ["name", "data", "size"]);
const release = embedded("ReleaseWebFont", ["name"]);
const pending = [];
global.FontFace = class {
  constructor(family, bytes) {
    this.family = family;
    this.bytes = bytes;
    this.promise = new Promise((resolve, reject) => {
      pending.push({ font: this, resolve, reject });
    });
  }
  load() { return this.promise; }
};
let notifications = 0;
let failures = 0;
global.Module = { _huxerui_web_fonts_changed: () => ++notifications };
global.document = { fonts: new Set() };
global.UTF8ToString = name => name;
global.HEAPU8 = new Uint8Array([1, 2, 3, 4]);
console.error = () => ++failures;
const flush = () => new Promise(resolve => setImmediate(resolve));

(async () => {
  assert.equal(prepare("regular", 0, 3), false);
  HEAPU8[0] = 9;
  assert.deepEqual(pending[0].font.bytes, new Uint8Array([1, 2, 3]));
  assert.equal(prepare("regular", 0, 3), false);
  assert.equal(pending.length, 1);
  assert.equal(prepare("bold", 0, 3), false);
  pending[0].resolve();
  pending[1].resolve();
  await flush();
  assert.equal(notifications, 1);
  assert.equal(document.fonts.size, 2);
  assert.equal(prepare("regular", 0, 3), true);
  release("regular");
  assert.equal(document.fonts.size, 1);

  prepare("retired", 0, 3);
  release("retired");
  pending[2].resolve();
  await flush();
  assert.equal(notifications, 1);
  assert.equal(document.fonts.size, 1);

  prepare("failed", 0, 3);
  pending[3].reject(new Error("invalid font"));
  await flush();
  assert.equal(failures, 1);
  assert.equal(prepare("failed", 0, 3), false);
  assert.equal(pending.length, 4);
  assert.equal(notifications, 1);

  prepare("old application", 0, 3);
  delete Module.huxerUIFontFaces;
  prepare("new application", 0, 3);
  pending[4].resolve();
  await flush();
  assert.equal(notifications, 1);
  assert.equal(document.fonts.size, 1);
  pending[5].resolve();
  await flush();
  assert.equal(notifications, 2);
  console.log("Web font readiness, failure, release and application replacement passed");
})().catch(error => { process.stderr.write(String(error.stack) + "\n"); process.exitCode = 1; });
