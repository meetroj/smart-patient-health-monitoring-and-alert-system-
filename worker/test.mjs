import assert from "node:assert";
import { alertMessages } from "./alert.js";
const now = 1e12, ok = { ts: now, bpm: 80, bpmStatus: "NORMAL", temperature: 36.6, tempStatus: "NORMAL" };
assert.deepEqual(alertMessages(ok, now), []);
assert.equal(alertMessages({ ...ok, tempStatus: "FEVER", temperature: 39 }, now).length, 1);
assert.equal(alertMessages({ ...ok, bpmStatus: "HIGH", tempStatus: "FEVER" }, now).length, 2);
assert.deepEqual(alertMessages({ ...ok, tempStatus: "FEVER", ts: now - 999999 }, now), []);   // stale device
assert.deepEqual(alertMessages(null, now), []);
console.log("ok");
