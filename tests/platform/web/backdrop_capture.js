"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const source = fs.readFileSync(process.argv[2], "utf8");
global.Emval = { toValue: (value) => value, toHandle: (value) => value };

const declaration = source.indexOf(", CaptureWebBackdrop,");
assert(declaration >= 0, "The Web renderer exports its backdrop capture bridge");
const start = source.indexOf(", {", declaration) + 3;
const end = source.indexOf("\n});", start);
assert(end > start, "The backdrop capture bridge has a complete body");
const capture = new Function("context_handle", "left", "top", "width", "height", source.slice(start, end));
const calls = [];
const image = {data: new Uint8ClampedArray([1, 2, 3, 4]), width: 1, height: 1};
const context = {
  getImageData(...bounds) {
    calls.push(bounds);
    return image;
  },
};

assert.equal(capture(context, 11, 13, 7, 9).image, image);
assert.deepEqual(calls, [[11, 13, 7, 9]], "Capture reads precisely the current cropped region");
context.isContextLost = () => true;
assert.match(capture(context, 0, 0, 1, 1).error, /^HuxerUI Web backdrop Canvas context is lost$/);
assert.equal(calls.length, 1, "A lost context is never read");
context.isContextLost = () => false;
context.getImageData = () => {throw new Error("Canvas is not origin-clean");};
const failed = capture(context, 0, 0, 1, 1);
assert.match(failed.error, /^HuxerUI Web backdrop capture failed: Canvas is not origin-clean$/);
assert.equal(failed.image, undefined, "An unreadable source does not return stale image data");
context.getImageData = () => image;
assert.equal(capture(context, 0, 0, 1, 1).image, image, "Capture recovers after the context becomes readable");
console.log("HuxerUI Web backdrop capture checks passed");
