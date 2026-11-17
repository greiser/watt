// The JavaScript side of request scripts (core/script.h): Bruno's bru / req / res / test / expect,
// Postman's pm.*, console, crypto, CryptoJS, Buffer, timers. It is evaluated once per process to
// a function (compiled to bytecode, then loaded into each fresh context) and called with the
// native host object H, the request data REQ and the phase ("pre" / "post"). H is reachable only
// through the closures defined here, never from the global object.
// MSVC caps one string literal at 16 KB, hence the chunks.
#include <cstddef>

namespace watt::detail {

extern const char* const kScriptPrelude[];
extern const size_t kScriptPreludeChunks;

const char* const kScriptPrelude[] = {
R"JS((function (H, REQ, PHASE) {
"use strict";
const G = globalThis;
const define = (name, value) =>
  Object.defineProperty(G, name, { value, writable: true, configurable: true, enumerable: false });
// Built on first use: each run starts from a fresh context and most scripts touch only a few of
// the APIs, so building all of them up front would dominate a short script's cost.
const lazy = (name, make) =>
  Object.defineProperty(G, name, {
    configurable: true, enumerable: false,
    get() { const v = make(); define(name, v); return v; },
    set(v) { define(name, v); },
  });

// ------------------------------------------------------------------ formatting
function fmt(v) {
  if (typeof v === "string") return v;
  if (v === undefined) return "undefined";
  if (v === null || typeof v === "number" || typeof v === "boolean" || typeof v === "bigint" || typeof v === "symbol")
    return String(v);
  if (typeof v === "function") return "[Function" + (v.name ? ": " + v.name : "") + "]";
  if (v instanceof Error) return (v.name || "Error") + ": " + v.message;
  if (ArrayBuffer.isView(v)) return v.constructor.name + "(" + v.length + ")";
  try {
    const s = JSON.stringify(v, (k, x) => (typeof x === "bigint" ? x.toString() : x));
    return s === undefined ? String(v) : s;
  } catch (e) {
    return String(v);
  }
}
function show(v) {
  let s = typeof v === "string" ? JSON.stringify(v) : fmt(v);
  if (s.length > 200) s = s.slice(0, 197) + "...";
  return s;
}
function errText(e) {
  if (e instanceof Error) return e.name === "AssertionError" ? e.message : (e.name || "Error") + ": " + e.message;
  return fmt(e);
}
const mkLog = (lvl) => function (...a) { H.log(lvl, a.map(fmt).join(" ")); };
define("console", Object.freeze({
  log: mkLog(0), info: mkLog(1), warn: mkLog(2), error: mkLog(3), debug: mkLog(0), trace: mkLog(0),
}));

const toStr = (v) =>
  typeof v === "string" ? v : v === undefined ? "" : v !== null && typeof v === "object" ? JSON.stringify(v) : String(v);

// ------------------------------------------------------------------ variables
function interpolate(t) {
  if (typeof t !== "string") return t;
  return t.replace(/\{\{\s*([^{}]+?)\s*\}\}/g, (m, n) => {
    const v = H.get("a", n);
    return v === undefined ? m : v;
  });
}
function scope(s) {
  return Object.freeze({
    get: (n) => H.get(s, String(n)),
    set: (n, v) => { H.set(s, String(n), toStr(v)); },
    has: (n) => H.has(s, String(n)),
    unset: (n) => { H.unset(s, String(n)); },
    clear: () => { for (const k of Object.keys(H.all(s))) H.unset(s, k); },
    toObject: () => H.all(s),
    replaceIn: (t) => interpolate(t),
    get name() { return s === "e" ? H.envName() : undefined; },
  });
}

// ------------------------------------------------------------------ deep equality, types
function deq(a, b, depth) {
  if (Object.is(a, b)) return true;
  if (typeof a !== "object" || typeof b !== "object" || a === null || b === null) return false;
  if ((depth = (depth || 0) + 1) > 200) throw new RangeError("eql: structure too deep");
  if (Array.isArray(a) !== Array.isArray(b)) return false;
  if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
  if (a instanceof Date) return a.getTime() === b.getTime();
  if (a instanceof RegExp) return String(a) === String(b);
  if (a instanceof Map || a instanceof Set) {
    if (a.size !== b.size) return false;
    const ea = [...a.entries()], eb = [...b.entries()];
    return ea.every((x, i) => deq(x, eb[i], depth));
  }
  if (ArrayBuffer.isView(a)) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false;
    return true;
  }
  const ka = Object.keys(a), kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  for (const k of ka) if (!Object.prototype.hasOwnProperty.call(b, k) || !deq(a[k], b[k], depth)) return false;
  return true;
}
function typeName(v) {
  if (v === null) return "null";
  if (Array.isArray(v)) return "array";
  if (v instanceof Date) return "date";
  if (v instanceof RegExp) return "regexp";
  if (v instanceof Map) return "map";
  if (v instanceof Set) return "set";
  if (v instanceof Promise) return "promise";
  if (v instanceof Error) return "error";
  if (ArrayBuffer.isView(v)) return v.constructor.name.toLowerCase();
  return typeof v;
}
function sizeOf(v) {
  if (v === null || v === undefined) return undefined;
  if (typeof v === "string" || Array.isArray(v) || ArrayBuffer.isView(v)) return v.length;
  if (v instanceof Map || v instanceof Set) return v.size;
  if (typeof v === "object") return typeof v.length === "number" ? v.length : Object.keys(v).length;
  return undefined;
}

class AssertionError extends Error {
  constructor(m) { super(m); this.name = "AssertionError"; }
}
define("AssertionError", AssertionError);
)JS",
R"JS(
// ------------------------------------------------------------------ expect (chai subset)
let expectFn;
function makeExpect() {
class Assertion {
  constructor(obj, msg) { this._obj = obj; this._msg = msg; this._neg = false; this._deep = false; this._own = false; this._any = false; }
  _ok(ok, m, mneg) {
    if (this._neg ? ok : !ok) throw new AssertionError((this._msg ? this._msg + ": " : "") + (this._neg ? mneg : m));
    return this;
  }
  get to() { return this; } get be() { return this; } get been() { return this; } get is() { return this; }
  get that() { return this; } get which() { return this; } get and() { return this; } get has() { return this; }
  get have() { return this; } get with() { return this; } get at() { return this; } get of() { return this; }
  get same() { return this; } get does() { return this; } get still() { return this; } get but() { return this; }
  get also() { return this; } get nested() { return this; } get ordered() { return this; }
  get not() { this._neg = !this._neg; return this; }
  get deep() { this._deep = true; return this; }
  get own() { this._own = true; return this; }
  get any() { this._any = true; return this; }
  get all() { this._any = false; return this; }
  get ok() { return this._ok(!!this._obj, `expected ${show(this._obj)} to be truthy`, `expected ${show(this._obj)} to be falsy`); }
  get true() { return this._ok(this._obj === true, `expected ${show(this._obj)} to be true`, `expected ${show(this._obj)} not to be true`); }
  get false() { return this._ok(this._obj === false, `expected ${show(this._obj)} to be false`, `expected ${show(this._obj)} not to be false`); }
  get null() { return this._ok(this._obj === null, `expected ${show(this._obj)} to be null`, `expected ${show(this._obj)} not to be null`); }
  get undefined() { return this._ok(this._obj === undefined, `expected ${show(this._obj)} to be undefined`, `expected ${show(this._obj)} not to be undefined`); }
  get NaN() { return this._ok(Number.isNaN(this._obj), `expected ${show(this._obj)} to be NaN`, `expected ${show(this._obj)} not to be NaN`); }
  get exist() { return this._ok(this._obj !== null && this._obj !== undefined, `expected ${show(this._obj)} to exist`, `expected ${show(this._obj)} to not exist`); }
  get exists() { return this.exist; }
  get empty() {
    const n = sizeOf(this._obj);
    return this._ok(n === 0, `expected ${show(this._obj)} to be empty`, `expected ${show(this._obj)} not to be empty`);
  }
  equal(v, msg) {
    if (msg) this._msg = msg;
    const ok = this._deep ? deq(this._obj, v) : this._obj === v;
    return this._ok(ok, `expected ${show(this._obj)} to ${this._deep ? "deeply " : ""}equal ${show(v)}`,
                    `expected ${show(this._obj)} to not ${this._deep ? "deeply " : ""}equal ${show(v)}`);
  }
  equals(v, m) { return this.equal(v, m); }
  eq(v, m) { return this.equal(v, m); }
  eql(v, msg) {
    if (msg) this._msg = msg;
    return this._ok(deq(this._obj, v), `expected ${show(this._obj)} to deeply equal ${show(v)}`, `expected ${show(this._obj)} to not deeply equal ${show(v)}`);
  }
  eqls(v, m) { return this.eql(v, m); }
  a(type, msg) {
    if (msg) this._msg = msg;
    const t = String(type).toLowerCase(), got = typeName(this._obj);
    const art = /^[aeiou]/.test(t) ? "an" : "a";
    return this._ok(got === t, `expected ${show(this._obj)} to be ${art} ${t}`, `expected ${show(this._obj)} not to be ${art} ${t}`);
  }
  an(type, msg) { return this.a(type, msg); }
  instanceOf(c, msg) {
    if (msg) this._msg = msg;
    return this._ok(this._obj instanceof c, `expected ${show(this._obj)} to be an instance of ${c && c.name}`, `expected ${show(this._obj)} not to be an instance of ${c && c.name}`);
  }
  instanceof(c, m) { return this.instanceOf(c, m); }
  include(v, msg) {
    if (msg) this._msg = msg;
    const o = this._obj;
    let ok = false;
    if (typeof o === "string") ok = o.indexOf(String(v)) >= 0;
    else if (Array.isArray(o)) ok = o.some((x) => (this._deep ? deq(x, v) : x === v));
    else if (o instanceof Set) ok = o.has(v);
    else if (o instanceof Map) ok = [...o.values()].some((x) => (this._deep ? deq(x, v) : x === v));
    else if (o !== null && typeof o === "object" && v !== null && typeof v === "object")
      ok = Object.keys(v).every((k) => k in o && (this._deep ? deq(o[k], v[k]) : o[k] === v[k]));
    else if (o !== null && typeof o === "object") ok = Object.values(o).some((x) => x === v);
    return this._ok(ok, `expected ${show(o)} to include ${show(v)}`, `expected ${show(o)} to not include ${show(v)}`);
  }
  includes(v, m) { return this.include(v, m); }
  contain(v, m) { return this.include(v, m); }
  contains(v, m) { return this.include(v, m); }
  string(v, m) { return this.include(v, m); }
  property(name, ...rest) {
    const o = this._obj;
    const has = o !== null && o !== undefined &&
      (this._own ? Object.prototype.hasOwnProperty.call(o, name) : typeof o === "object" || typeof o === "function" ? name in o : o[name] !== undefined);
    if (rest.length === 0 || this._neg && !has) {
      this._ok(has, `expected ${show(o)} to have ${this._own ? "own " : ""}property ${show(name)}`, `expected ${show(o)} to not have property ${show(name)}`);
    } else {
      const v = rest[0], actual = has ? o[name] : undefined;
      const ok = has && (this._deep ? deq(actual, v) : actual === v);
      this._ok(ok, `expected ${show(o)} to have property ${show(name)} of ${show(v)}, but got ${show(actual)}`,
               `expected ${show(o)} to not have property ${show(name)} of ${show(v)}`);
    }
    if (!this._neg && has) this._obj = o[name];
    return this;
  }
  ownProperty(name, ...rest) { this._own = true; return this.property(name, ...rest); }
  haveOwnProperty(name, ...rest) { return this.ownProperty(name, ...rest); }
  keys(...ks) {
    if (ks.length === 1 && Array.isArray(ks[0])) ks = ks[0];
    else if (ks.length === 1 && ks[0] !== null && typeof ks[0] === "object") ks = Object.keys(ks[0]);
    const o = this._obj;
    const own = o !== null && typeof o === "object" ? (o instanceof Map ? [...o.keys()] : Object.keys(o)) : [];
    const ok = this._any ? ks.some((k) => own.includes(k)) : ks.every((k) => own.includes(k)) && (this._neg || own.length === ks.length);
    return this._ok(ok, `expected ${show(o)} to have ${this._any ? "any of " : ""}keys ${show(ks)}`, `expected ${show(o)} to not have keys ${show(ks)}`);
  }
  key(...ks) { return this.keys(...ks); }
  match(re, msg) {
    if (msg) this._msg = msg;
    const r = re instanceof RegExp ? re : new RegExp(String(re));
    return this._ok(r.test(String(this._obj)), `expected ${show(this._obj)} to match ${r}`, `expected ${show(this._obj)} not to match ${r}`);
  }
  matches(re, m) { return this.match(re, m); }
  _num(op, n, words) {
    const o = this._lenMode ? sizeOf(this._obj) : this._obj;
    const what = this._lenMode ? `expected ${show(this._obj)} to have a length ` : `expected ${show(o)} to be `;
    return this._ok(op(o, n), what + words + " " + show(n) + (this._lenMode ? " but got " + o : ""), what.replace(" to ", " to not ") + words + " " + show(n));
  }
  above(n, m) { if (m) this._msg = m; return this._num((a, b) => a > b, n, "above"); }
  gt(n, m) { return this.above(n, m); } greaterThan(n, m) { return this.above(n, m); }
  below(n, m) { if (m) this._msg = m; return this._num((a, b) => a < b, n, "below"); }
  lt(n, m) { return this.below(n, m); } lessThan(n, m) { return this.below(n, m); }
  least(n, m) { if (m) this._msg = m; return this._num((a, b) => a >= b, n, "at least"); }
  gte(n, m) { return this.least(n, m); } greaterThanOrEqual(n, m) { return this.least(n, m); }
  most(n, m) { if (m) this._msg = m; return this._num((a, b) => a <= b, n, "at most"); }
  lte(n, m) { return this.most(n, m); } lessThanOrEqual(n, m) { return this.most(n, m); }
  within(lo, hi, m) {
    if (m) this._msg = m;
    const o = this._lenMode ? sizeOf(this._obj) : this._obj;
    return this._ok(o >= lo && o <= hi, `expected ${show(o)} to be within ${lo}..${hi}`, `expected ${show(o)} to not be within ${lo}..${hi}`);
  }
  closeTo(n, delta, m) {
    if (m) this._msg = m;
    return this._ok(Math.abs(this._obj - n) <= delta, `expected ${show(this._obj)} to be close to ${n} +/- ${delta}`, `expected ${show(this._obj)} not to be close to ${n} +/- ${delta}`);
  }
  approximately(n, d, m) { return this.closeTo(n, d, m); }
  get length() { this._lenMode = true; const self = this; const f = (n, m) => self.lengthOf(n, m); Object.setPrototypeOf(f, self); return new Proxy(f, { get: (t, p) => { const v = self[p]; return typeof v === "function" ? v.bind(self) : v; } }); }
  lengthOf(n, m) {
    if (m) this._msg = m;
    const got = sizeOf(this._obj);
    return this._ok(got === n, `expected ${show(this._obj)} to have a length of ${n} but got ${got}`, `expected ${show(this._obj)} to not have a length of ${n}`);
  }
  oneOf(list, m) {
    if (m) this._msg = m;
    const ok = Array.isArray(list) && list.some((x) => (this._deep ? deq(x, this._obj) : x === this._obj));
    return this._ok(ok, `expected ${show(this._obj)} to be one of ${show(list)}`, `expected ${show(this._obj)} to not be one of ${show(list)}`);
  }
  members(list, m) {
    if (m) this._msg = m;
    const o = this._obj;
    const ok = Array.isArray(o) && Array.isArray(list) && o.length === list.length &&
      list.every((x) => o.some((y) => (this._deep ? deq(x, y) : x === y)));
    return this._ok(ok, `expected ${show(o)} to have the same members as ${show(list)}`, `expected ${show(o)} to not have the same members as ${show(list)}`);
  }
  satisfy(fn, m) {
    if (m) this._msg = m;
    return this._ok(!!fn(this._obj), `expected ${show(this._obj)} to satisfy ${fmt(fn)}`, `expected ${show(this._obj)} to not satisfy ${fmt(fn)}`);
  }
  satisfies(fn, m) { return this.satisfy(fn, m); }
  throw(what, m) {
    if (m) this._msg = m;
    let thrown = false, err;
    try { this._obj(); } catch (e) { thrown = true; err = e; }
    let ok = thrown;
    if (thrown && what !== undefined) {
      if (typeof what === "string") ok = err instanceof Error ? err.message.includes(what) : String(err).includes(what);
      else if (what instanceof RegExp) ok = what.test(err instanceof Error ? err.message : String(err));
      else if (typeof what === "function") ok = err instanceof what;
    }
    return this._ok(ok, `expected [Function] to throw${what !== undefined ? " " + show(what) : ""}`, `expected [Function] to not throw${thrown ? " but it threw " + show(errText(err)) : ""}`);
  }
  throws(w, m) { return this.throw(w, m); }
  Throw(w, m) { return this.throw(w, m); }
  status(code) {
    const o = this._obj;
    const got = o && (o.code !== undefined ? o.code : o.status);
    return this._ok(got === code, `expected response to have status code ${code} but got ${got}`, `expected response to not have status code ${code}`);
  }
}
function expect(v, msg) { return new Assertion(v, msg); }
expect.fail = (m) => { throw new AssertionError(m || "expect.fail()"); };
return expect;
}
const getExpect = () => expectFn || (expectFn = makeExpect());
lazy("expect", getExpect);
)JS",
R"JS(
// ------------------------------------------------------------------ tests
function test(name, fn) {
  name = String(name);
  if (typeof fn !== "function") { H.test(name, false, "test(name, fn): fn must be a function"); return; }
  try {
    const r = fn();
    if (r && typeof r.then === "function")
      r.then(() => H.test(name, true, ""), (e) => H.test(name, false, errText(e)));
    else H.test(name, true, "");
  } catch (e) {
    H.test(name, false, errText(e));
  }
}
test.skip = (name) => H.log(1, "test skipped: " + String(name));
define("test", test);
define("it", test);

// ------------------------------------------------------------------ bytes, crypto
const utf8 = (s) => new Uint8Array(H.utf8(String(s)));
function latin1(u8) {
  let s = "";
  for (let i = 0; i < u8.length; i += 4096) s += String.fromCharCode.apply(null, u8.subarray(i, i + 4096));
  return s;
}
function fromLatin1(s) {
  s = String(s);
  const a = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 255;
  return a;
}
function toU8(d) {
  if (typeof d === "string") return utf8(d);
  if (d && d._u8 instanceof Uint8Array) return d._u8;  // a CryptoJS WordArray
  if (d instanceof Uint8Array) return d;
  if (d instanceof ArrayBuffer) return new Uint8Array(d);
  if (ArrayBuffer.isView(d)) return new Uint8Array(d.buffer, d.byteOffset, d.byteLength);
  if (d === undefined || d === null) return new Uint8Array(0);
  return utf8(String(d));
}
// Natives take a plain Uint8Array (a subarray / subclass view is copied).
const raw = (d) => { const u = toU8(d); return u.constructor === Uint8Array ? u : new Uint8Array(u); };
function encode(u8, enc) {
  switch (String(enc === undefined ? "hex" : enc).toLowerCase()) {
    case "hex": return H.hex(raw(u8));
    case "base64": return H.b64(raw(u8), false);
    case "base64url": return H.b64(raw(u8), true);
    case "binary": case "latin1": return latin1(u8);
    case "utf8": case "utf-8": return H.utf8dec(raw(u8));
    case "bytes": case "buffer": return u8;
    default: throw new Error("unknown encoding " + enc);
  }
}
function decode(s, enc) {
  s = String(s);
  switch (String(enc || "utf8").toLowerCase()) {
    case "base64": case "base64url": { const b = H.unb64(s); if (b === null) throw new Error("invalid base64"); return new Uint8Array(b); }
    case "hex": { const b = H.unhex(s); if (b === null) throw new Error("invalid hex"); return new Uint8Array(b); }
    case "binary": case "latin1": return fromLatin1(s);
    default: return utf8(s);
  }
}
const dig = (alg) => (d, e) => encode(new Uint8Array(H.digest(alg, raw(d))), e);
lazy("crypto", () => Object.freeze({
  md5: dig("md5"), sha1: dig("sha1"), sha256: dig("sha256"), sha384: dig("sha384"), sha512: dig("sha512"),
  hash: (alg, d, e) => encode(new Uint8Array(H.digest(String(alg), raw(d))), e),
  hmac: (alg, key, d, e) => encode(new Uint8Array(H.hmac(String(alg), raw(key), raw(d))), e),
  base64: (d) => H.b64(raw(d), false),
  base64url: (d) => H.b64(raw(d), true),
  base64Decode: (s, enc) => (enc === "bytes" ? decode(s, "base64") : H.utf8dec(decode(s, "base64"))),
  hex: (d) => H.hex(raw(d)),
  hexDecode: (s, enc) => (enc === "bytes" ? decode(s, "hex") : H.utf8dec(decode(s, "hex"))),
  randomUUID: () => H.uuid(),
  randomBytes: (n) => new Uint8Array(H.random(n >>> 0)),
  getRandomValues: (a) => {
    if (!ArrayBuffer.isView(a)) throw new TypeError("getRandomValues needs a typed array");
    new Uint8Array(a.buffer, a.byteOffset, a.byteLength).set(new Uint8Array(H.random(a.byteLength)));
    return a;
  },
}));

let cjs;
function makeCryptoJS() {
class WordArray {
  constructor(u8) { this._u8 = u8 instanceof Uint8Array ? u8 : new Uint8Array(0); }
  get sigBytes() { return this._u8.length; }
  get words() {
    const w = [];
    for (let i = 0; i < this._u8.length; i++) w[i >>> 2] |= this._u8[i] << (24 - (i % 4) * 8);
    return w;
  }
  toString(encoder) { return (encoder || Hex).stringify(this); }
  concat(o) {
    const b = toU8(o), a = new Uint8Array(this._u8.length + b.length);
    a.set(this._u8);
    a.set(b, this._u8.length);
    this._u8 = a;
    return this;
  }
  clone() { return new WordArray(this._u8.slice()); }
  static create(words, sigBytes) {
    if (words instanceof Uint8Array) return new WordArray(words.slice());
    if (words instanceof ArrayBuffer) return new WordArray(new Uint8Array(words.slice(0)));
    words = Array.isArray(words) ? words : [];
    const n = sigBytes === undefined ? words.length * 4 : sigBytes;
    const a = new Uint8Array(n);
    for (let i = 0; i < n; i++) a[i] = (words[i >>> 2] >>> (24 - (i % 4) * 8)) & 255;
    return new WordArray(a);
  }
  static random(n) { return new WordArray(new Uint8Array(H.random(n >>> 0))); }
}
const Hex = { stringify: (wa) => H.hex(raw(wa)), parse: (s) => new WordArray(decode(s, "hex")) };
const Base64 = { stringify: (wa) => H.b64(raw(wa), false), parse: (s) => new WordArray(decode(s, "base64")) };
const Base64url = { stringify: (wa) => H.b64(raw(wa), true), parse: (s) => new WordArray(decode(s, "base64")) };
const Utf8 = { stringify: (wa) => H.utf8dec(raw(wa)), parse: (s) => new WordArray(utf8(s)) };
const Latin1 = { stringify: (wa) => latin1(toU8(wa)), parse: (s) => new WordArray(fromLatin1(s)) };
const hasher = (alg) => (m) => new WordArray(new Uint8Array(H.digest(alg, raw(m))));
const hmacer = (alg) => (m, k) => new WordArray(new Uint8Array(H.hmac(alg, raw(k), raw(m))));
const unsupported = (n) => ({ encrypt() { throw new Error("CryptoJS." + n + " is not available in watt scripts"); },
                              decrypt() { throw new Error("CryptoJS." + n + " is not available in watt scripts"); } });
const CryptoJS = Object.freeze({
  lib: { WordArray },
  enc: { Hex, Base64, Base64url, Utf8, Latin1 },
  MD5: hasher("md5"), SHA1: hasher("sha1"), SHA256: hasher("sha256"), SHA384: hasher("sha384"), SHA512: hasher("sha512"),
  HmacMD5: hmacer("md5"), HmacSHA1: hmacer("sha1"), HmacSHA256: hmacer("sha256"), HmacSHA384: hmacer("sha384"),
  HmacSHA512: hmacer("sha512"),
  AES: unsupported("AES"), DES: unsupported("DES"), TripleDES: unsupported("TripleDES"), RC4: unsupported("RC4"),
});
return CryptoJS;
}
const getCryptoJS = () => cjs || (cjs = makeCryptoJS());
lazy("CryptoJS", getCryptoJS);

lazy("Buffer", () => {
class Buffer extends Uint8Array {
  static from(d, enc) {
    const u = typeof d === "string" ? decode(d, enc) : Array.isArray(d) ? Uint8Array.from(d) : toU8(d);
    const b = new Buffer(u.length);
    b.set(u);
    return b;
  }
  static isBuffer(x) { return x instanceof Buffer; }
  static byteLength(s, enc) { return typeof s === "string" ? decode(s, enc).length : toU8(s).length; }
  static concat(list) {
    const n = list.reduce((a, x) => a + x.length, 0), b = new Buffer(n);
    let o = 0;
    for (const x of list) { b.set(x, o); o += x.length; }
    return b;
  }
  toString(enc) { return encode(this, enc || "utf8"); }
}
return Buffer;
});

// ------------------------------------------------------------------ timers (run after the script, no waiting)
const timers = [];
let timerSeq = 0, timerNow = 0;
define("setTimeout", function (fn, ms, ...args) {
  if (typeof fn !== "function") throw new TypeError("setTimeout(fn, ms): fn must be a function (strings are not evaluated)");
  const id = ++timerSeq;
  timers.push({ id, at: timerNow + Math.max(0, Number(ms) || 0), fn, args });
  return id;
});
define("clearTimeout", (id) => { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); });
define("setImmediate", (fn, ...a) => G.setTimeout(fn, 0, ...a));
define("setInterval", () => { throw new Error("setInterval is not available in watt scripts"); });
define("clearInterval", () => {});
function drainOne() {
  if (!timers.length) return false;
  let b = 0;
  for (let i = 1; i < timers.length; i++)
    if (timers[i].at < timers[b].at || (timers[i].at === timers[b].at && timers[i].id < timers[b].id)) b = i;
  const t = timers.splice(b, 1)[0];
  timerNow = t.at;
  t.fn(...t.args);
  return true;
}
)JS",
R"JS(
// ------------------------------------------------------------------ request
const lc = (s) => String(s).toLowerCase();
function hdrFind(list, n) { n = lc(n); for (const h of list) if (lc(h[0]) === n) return h; return undefined; }
function hdrSet(list, n, v) {
  n = String(n); v = toStr(v);
  const l = lc(n);
  let done = false;
  for (let i = 0; i < list.length;) {
    if (lc(list[i][0]) === l) {
      if (!done) { list[i] = [list[i][0], v]; done = true; i++; } else list.splice(i, 1);
    } else i++;
  }
  if (!done) list.push([n, v]);
}
function hdrDel(list, n) { const l = lc(n); for (let i = list.length - 1; i >= 0; i--) if (lc(list[i][0]) === l) list.splice(i, 1); }
function hdrObj(list) { const o = {}; for (const [k, v] of list) o[k] = o[k] === undefined ? v : o[k] + ", " + v; return o; }
function hdrObjLower(list) { const o = {}; for (const [k, v] of list) { const l = lc(k); o[l] = o[l] === undefined ? v : o[l] + ", " + v; } return o; }

// Query parameters edited in place: untouched ones keep their exact text; {{vars}} stay unencoded.
function encQ(s) {
  s = String(s);
  let out = "", i = 0, m;
  const re = /\{\{[^{}]*\}\}/g;
  while ((m = re.exec(s))) { out += encodeURIComponent(s.slice(i, m.index)) + m[0]; i = m.index + m[0].length; }
  return out + encodeURIComponent(s.slice(i));
}
function dec(s) { try { return decodeURIComponent(s.replace(/\+/g, " ")); } catch (e) { return s; } }
function urlParts() {
  let u = REQ.url;
  const h = u.indexOf("#"), frag = h >= 0 ? u.slice(h) : "";
  if (h >= 0) u = u.slice(0, h);
  const q = u.indexOf("?");
  const segs = q < 0 ? [] : u.slice(q + 1).split("&").filter((p) => p.length).map((rawSeg) => {
    const e = rawSeg.indexOf("=");
    return { raw: rawSeg, key: dec(e < 0 ? rawSeg : rawSeg.slice(0, e)), value: e < 0 ? null : dec(rawSeg.slice(e + 1)) };
  });
  return { base: q < 0 ? u : u.slice(0, q), segs, frag };
}
function urlJoin(p) { REQ.url = p.base + (p.segs.length ? "?" + p.segs.map((s) => s.raw).join("&") : "") + p.frag; }
const seg = (k, v) => ({ raw: v === null || v === undefined ? encQ(k) : encQ(k) + "=" + encQ(toStr(v)), key: String(k), value: v === null || v === undefined ? null : toStr(v) });
function qGet(k) { const s = urlParts().segs.find((x) => x.key === String(k)); return s ? (s.value === null ? "" : s.value) : undefined; }
function qSet(k, v) {
  const p = urlParts(), i = p.segs.findIndex((x) => x.key === String(k));
  if (i >= 0) { p.segs[i] = seg(k, v); p.segs = p.segs.filter((x, j) => j <= i || x.key !== String(k)); } else p.segs.push(seg(k, v));
  urlJoin(p);
}
function qAdd(k, v) { const p = urlParts(); p.segs.push(seg(k, v)); urlJoin(p); }
function qDel(k) { const p = urlParts(); p.segs = p.segs.filter((x) => x.key !== String(k)); urlJoin(p); }
function parseBody(b) {
  const t = String(b).trim();
  if (t.startsWith("{") || t.startsWith("[")) { try { return JSON.parse(t); } catch (e) {} }
  return b;
}

let reqObj;
function makeReq() {
const req = {
  getUrl: () => REQ.url, setUrl: (u) => { REQ.url = String(u); },
  getMethod: () => REQ.method, setMethod: (m) => { REQ.method = String(m); },
  getHeader: (n) => { const h = hdrFind(REQ.headers, n); return h ? h[1] : undefined; },
  getHeaders: () => hdrObj(REQ.headers),
  setHeader: (n, v) => hdrSet(REQ.headers, n, v),
  setHeaders: (o) => { REQ.headers.length = 0; for (const k of Object.keys(o || {})) REQ.headers.push([String(k), toStr(o[k])]); },
  deleteHeader: (n) => hdrDel(REQ.headers, n),
  getBody: () => parseBody(REQ.body),
  setBody: (b) => { REQ.body = typeof b === "string" ? b : JSON.stringify(b); },
  getName: () => REQ.name,
  getTimeout: () => REQ.timeout, setTimeout: (ms) => { REQ.timeout = Number(ms) || 0; },
  getQueryParam: qGet, setQueryParam: qSet, deleteQueryParam: qDel,
  get url() { return REQ.url; }, set url(u) { REQ.url = String(u); },
  get method() { return REQ.method; }, set method(m) { REQ.method = String(m); },
  get headers() { return hdrObj(REQ.headers); },
  get body() { return parseBody(REQ.body); }, set body(b) { req.setBody(b); },
  get name() { return REQ.name; },
};
return req;
}
const getReq = () => reqObj || (reqObj = makeReq());
lazy("req", getReq);

const keyOf = (h) => (h !== null && typeof h === "object" ? h.key : h);
function makeReqLists() {
const headerList = Object.freeze({
  add: (h) => {
    if (typeof h === "string") { const i = h.indexOf(":"); h = { key: i < 0 ? h : h.slice(0, i), value: i < 0 ? "" : h.slice(i + 1).trim() }; }
    if (h && !h.disabled) REQ.headers.push([String(h.key), toStr(h.value === undefined ? "" : h.value)]);
  },
  upsert: (h) => { if (h && !h.disabled) hdrSet(REQ.headers, h.key, h.value === undefined ? "" : h.value); },
  remove: (n) => hdrDel(REQ.headers, keyOf(n)),
  get: (n) => { const h = hdrFind(REQ.headers, n); return h ? h[1] : undefined; },
  has: (n) => !!hdrFind(REQ.headers, keyOf(n)),
  all: () => REQ.headers.map(([key, value]) => ({ key, value })),
  toObject: () => hdrObj(REQ.headers),
  each: (fn) => REQ.headers.map(([key, value]) => ({ key, value })).forEach(fn),
  count: () => REQ.headers.length,
});
const queryList = Object.freeze({
  add: (q) => { if (q && !q.disabled) qAdd(q.key, q.value === undefined ? null : q.value); },
  upsert: (q) => { if (q && !q.disabled) qSet(q.key, q.value === undefined ? null : q.value); },
  remove: (k) => qDel(keyOf(k)),
  get: (k) => qGet(k),
  has: (k) => qGet(keyOf(k)) !== undefined,
  all: () => urlParts().segs.map((s) => ({ key: s.key, value: s.value })),
  toObject: () => { const o = {}; for (const s of urlParts().segs) o[s.key] = s.value; return o; },
  each: (fn) => urlParts().segs.map((s) => ({ key: s.key, value: s.value })).forEach(fn),
  count: () => urlParts().segs.length,
  toString: () => urlParts().segs.map((s) => s.raw).join("&"),
});
const urlObj = Object.freeze({
  toString: () => REQ.url,
  getRaw: () => REQ.url,
  update: (u) => { REQ.url = String(u); },
  getHost: () => { const m = /^[a-z][a-z0-9+.-]*:\/\/(?:[^@\/?#]*@)?(\[[^\]]*\]|[^:\/?#]*)/i.exec(REQ.url); return m ? m[1] : ""; },
  getPath: () => { const m = /^[a-z][a-z0-9+.-]*:\/\/[^\/?#]*([^?#]*)/i.exec(REQ.url); return m ? m[1] || "/" : ""; },
  getQueryString: () => queryList.toString(),
  query: queryList,
});
return { headerList, queryList, urlObj };
}
)JS",
R"JS(
// ------------------------------------------------------------------ response
const RES = PHASE === "post" ? H.res() : undefined;
let bodyDone = false, bodyVal;
const resBody = () => { if (!bodyDone) { bodyVal = H.resBody(false); bodyDone = true; } return bodyVal; };
let msgsDone = false, msgsVal;
const resMessages = () => { if (!msgsDone) { msgsVal = RES && RES.kind === "grpc" ? H.resMessages() : undefined; msgsDone = true; } return msgsVal; };
const resHeader = (list, n) => { const h = hdrFind(list || [], n); return h ? h[1] : undefined; };
function walk(v, path) {
  if (path === undefined || path === null || path === "") return v;
  const parts = String(path).match(/[^.[\]]+|\[(-?\d+)\]/g) || [];
  for (let p of parts) {
    if (v === null || v === undefined) return undefined;
    if (p.startsWith("[")) { const i = Number(p.slice(1, -1)); v = Array.isArray(v) ? v[i < 0 ? v.length + i : i] : undefined; }
    else v = v[p];
  }
  return v;
}
function respTo(r, neg) {
  const A = (ok, m, mn) => { if (neg ? ok : !ok) throw new AssertionError(neg ? mn : m); return o; };
  const code = () => r.code, reason = () => r.reason;
  const range = (lo, hi, w) => A(code() >= lo && code() <= hi, `expected response code to be ${w} but found ${code()}`, `expected response code not to be ${w} but found ${code()}`);
  const o = {
    get to() { return o; }, get be() { return o; }, get have() { return o; }, get and() { return o; }, get a() { return o; }, get an() { return o; },
    get not() { return respTo(r, !neg); },
    status: (c) => typeof c === "number"
      ? A(code() === c, `expected response to have status code ${c} but got ${code()}`, `expected response to not have status code ${c}`)
      : A(reason() === String(c), `expected response to have status reason ${show(c)} but got ${show(reason())}`, `expected response to not have status reason ${show(c)}`),
    header: (n, v) => {
      const got = r.header(n);
      if (v === undefined) return A(got !== undefined, `expected response to have header ${show(n)}`, `expected response to not have header ${show(n)}`);
      return A(got === String(v), `expected response header ${show(n)} to be ${show(v)} but got ${show(got)}`, `expected response header ${show(n)} to not be ${show(v)}`);
    },
    body: (s) => {
      const t = r.text();
      if (s === undefined) return A(t.length > 0, "expected response to have a body", "expected response to not have a body");
      return A(s instanceof RegExp ? s.test(t) : t === String(s), `expected response body to be ${show(s)}`, `expected response body to not be ${show(s)}`);
    },
    jsonBody: (path, v) => {
      let j, ok = true;
      try { j = r.json(); } catch (e) { ok = false; }
      if (!ok) return A(false, "expected response body to be valid JSON", "expected response body to not be valid JSON");
      if (path === undefined) return A(true, "", "expected response body to not be valid JSON");
      const got = walk(j, path);
      if (v === undefined) return A(got !== undefined, `expected response JSON to have ${show(path)}`, `expected response JSON to not have ${show(path)}`);
      return A(deq(got, v), `expected response JSON ${show(path)} to be ${show(v)} but got ${show(got)}`, `expected response JSON ${show(path)} to not be ${show(v)}`);
    },
    responseTime: undefined,
    get ok() { return range(200, 299, "2XX"); }, get success() { return range(200, 299, "2XX"); },
    get info() { return range(100, 199, "1XX"); }, get redirection() { return range(300, 399, "3XX"); },
    get clientError() { return range(400, 499, "4XX"); }, get serverError() { return range(500, 599, "5XX"); },
    get error() { return range(400, 599, "4XX or 5XX"); },
    get accepted() { return range(202, 202, "202"); }, get badRequest() { return range(400, 400, "400"); },
    get unauthorized() { return range(401, 401, "401"); }, get forbidden() { return range(403, 403, "403"); },
    get notFound() { return range(404, 404, "404"); }, get rateLimited() { return range(429, 429, "429"); },
    get json() { let ok = true; try { r.json(); } catch (e) { ok = false; } return A(ok, "expected response body to be valid JSON", "expected response body to not be valid JSON"); },
    get withBody() { return o.body(); },
  };
  return o;
}

function makeRes() {
if (!RES) return undefined;
const res = function (path) { return walk(res.getBody(), path); };
{
  Object.defineProperties(res, {
    status: { get: () => RES.status, enumerable: true }, statusText: { get: () => RES.statusText, enumerable: true },
    headers: { get: () => hdrObjLower(RES.headers), enumerable: true }, body: { get: resBody, enumerable: true },
    responseTime: { get: () => RES.time, enumerable: true }, url: { get: () => RES.url, enumerable: true },
    size: { get: () => RES.size, enumerable: true }, timings: { get: () => RES.timings, enumerable: true },
    statusMessage: { get: () => RES.statusMessage, enumerable: true },
    trailers: { get: () => (RES.trailers ? hdrObjLower(RES.trailers) : undefined), enumerable: true },
    messages: { get: resMessages, enumerable: true },
  });
  Object.assign(res, {
    getStatus: () => RES.status, getStatusText: () => RES.statusText, getHeader: (n) => resHeader(RES.headers, n),
    getHeaders: () => hdrObjLower(RES.headers), getBody: resBody, getResponseTime: () => RES.time,
    getSize: () => RES.size, getUrl: () => RES.url, getTimings: () => RES.timings, getMessages: resMessages,
    getTrailers: () => (RES.trailers ? hdrObjLower(RES.trailers) : undefined), getTrailer: (n) => resHeader(RES.trailers, n),
    getStatusMessage: () => RES.statusMessage, setBody: (b) => { bodyVal = b; bodyDone = true; },
  });
}
return res;
}
lazy("res", makeRes);
)JS",
R"JS(
// ------------------------------------------------------------------ sendRequest
function normalizeSend(o) {
  if (typeof o === "string") o = { url: o };
  if (!o || typeof o !== "object") throw new TypeError("sendRequest needs a URL or an options object");
  let url = o.url;
  if (url && typeof url === "object") url = url.raw !== undefined ? url.raw : String(url);
  const out = { url: String(url === undefined ? "" : url), method: String(o.method || "GET"), headers: [], body: "" };
  if (o.timeout !== undefined) out.timeout = Number(o.timeout) || 0;
  const hs = o.header !== undefined ? o.header : o.headers;
  if (Array.isArray(hs)) {
    for (const h of hs) {
      if (Array.isArray(h)) out.headers.push([String(h[0]), toStr(h[1])]);
      else if (typeof h === "string") { const i = h.indexOf(":"); out.headers.push([i < 0 ? h : h.slice(0, i), i < 0 ? "" : h.slice(i + 1).trim()]); }
      else if (h && typeof h === "object" && !h.disabled) out.headers.push([String(h.key), toStr(h.value === undefined ? "" : h.value)]);
    }
  } else if (hs && typeof hs === "object") {
    for (const k of Object.keys(hs)) out.headers.push([k, toStr(hs[k])]);
  }
  const b = o.body !== undefined ? o.body : o.data;
  const ctype = (t) => { if (!hdrFind(out.headers, "content-type")) out.headers.push(["Content-Type", t]); };
  if (b !== undefined && b !== null) {
    if (typeof b === "string") out.body = b;
    else if (b instanceof Uint8Array || b instanceof ArrayBuffer || ArrayBuffer.isView(b)) out.body = raw(b);
    else if (b.mode === "raw") out.body = toStr(b.raw === undefined ? "" : b.raw);
    else if (b.mode === "urlencoded") {
      out.body = (b.urlencoded || []).filter((p) => !p.disabled)
        .map((p) => encodeURIComponent(p.key) + "=" + encodeURIComponent(toStr(p.value === undefined ? "" : p.value))).join("&");
      ctype("application/x-www-form-urlencoded");
    } else if (b.mode === "formdata" || b.mode === "file")
      throw new Error("sendRequest: form-data and file bodies are not available in scripts (no file access)");
    else if (b.mode === "graphql") { out.body = JSON.stringify(b.graphql || {}); ctype("application/json"); }
    else { out.body = JSON.stringify(b); ctype("application/json"); }
  }
  return out;
}
const sentApi = (r) => ({
  code: r.status, reason: r.statusText,
  header: (n) => resHeader(r.headers, n), text: () => r.body, json: () => JSON.parse(r.body),
});
function pmResponse(r) {
  const api = sentApi(r);
  return {
    code: r.status, status: r.statusText, responseTime: r.timeMs, responseSize: r.size,
    headers: Object.freeze({
      get: (n) => resHeader(r.headers, n), has: (n) => resHeader(r.headers, n) !== undefined,
      all: () => r.headers.map(([key, value]) => ({ key, value })), toObject: () => hdrObj(r.headers),
      each: (fn) => r.headers.map(([key, value]) => ({ key, value })).forEach(fn),
    }),
    text: () => r.body, json: () => JSON.parse(r.body), reason: () => r.statusText,
    to: respTo(api, false),
  };
}
function bruResponse(r) {
  return { status: r.status, statusText: r.statusText, headers: hdrObjLower(r.headers), data: parseBody(r.body),
           responseTime: r.timeMs, getStatus: () => r.status, getHeader: (n) => resHeader(r.headers, n),
           getBody: () => parseBody(r.body), getResponseTime: () => r.timeMs };
}
// Transport failures and refused URLs go to the callback / the promise; misuse (bad options, the
// per-run request limit) throws.
function doSend(o, cb, wrap) {
  const r = H.send(normalizeSend(o));
  const err = r.error ? new Error(r.error) : null;
  const v = err ? undefined : wrap(r);
  if (typeof cb === "function") { cb(err, err ? null : v); return Promise.resolve(v); }
  return err ? Promise.reject(err) : Promise.resolve(v);
}

// ------------------------------------------------------------------ bru, pm
lazy("bru", () => {
const rt = scope("r"), env = scope("e"), glob = scope("g");
return Object.freeze({
  getEnvVar: env.get, setEnvVar: (n, v) => env.set(n, v), hasEnvVar: env.has, deleteEnvVar: env.unset,
  getVar: rt.get, setVar: (n, v) => rt.set(n, v), hasVar: rt.has, deleteVar: rt.unset, deleteAllVars: rt.clear,
  getGlobalEnvVar: glob.get, setGlobalEnvVar: (n, v) => glob.set(n, v),
  getCollectionVar: glob.get, getFolderVar: () => undefined, getRequestVar: () => undefined,
  getEnvName: () => H.envName(),
  getProcessEnv: (n) => { H.log(2, "bru.getProcessEnv(" + show(String(n)) + "): OS environment variables are not available to scripts"); return undefined; },
  interpolate,
  sendRequest: (o, cb) => doSend(o, cb, bruResponse),
  sleep: (ms) => { H.sleep(Number(ms) || 0); return Promise.resolve(); },
  setNextRequest: (n) => H.log(2, "bru.setNextRequest(" + show(String(n)) + ") is not supported"),
  runner: Object.freeze({ skipRequest: () => H.log(2, "bru.runner.skipRequest() is not supported"),
                          stopExecution: () => H.log(2, "bru.runner.stopExecution() is not supported") }),
  cwd: () => undefined,
});
});

lazy("pm", () => {
const env = scope("e"), glob = scope("g"), any = scope("a");
const { headerList, queryList, urlObj } = makeReqLists();
const pmRes = RES && {
  get code() { return RES.status; }, get status() { return RES.statusText; },
  get responseTime() { return RES.time; }, get responseSize() { return RES.size; },
  headers: Object.freeze({
    get: (n) => resHeader(RES.headers, n), has: (n) => resHeader(RES.headers, n) !== undefined,
    all: () => RES.headers.map(([key, value]) => ({ key, value })), toObject: () => hdrObj(RES.headers),
    each: (fn) => RES.headers.map(([key, value]) => ({ key, value })).forEach(fn), count: () => RES.headers.length,
  }),
  json: () => H.resBody(true), text: () => H.resText(), reason: () => RES.statusText,
  get messages() { return resMessages(); }, get trailers() { return RES.trailers ? hdrObj(RES.trailers) : undefined; },
};
if (pmRes) pmRes.to = respTo({ get code() { return RES.status; }, get reason() { return RES.statusText; },
                               header: (n) => resHeader(RES.headers, n), text: () => H.resText(), json: () => H.resBody(true) }, false);
const pm = {
  environment: env, variables: any, globals: glob, collectionVariables: glob,
  request: {
    get url() { return urlObj; }, set url(u) { REQ.url = String(u); },
    get method() { return REQ.method; }, set method(m) { REQ.method = String(m); },
    headers: headerList,
    body: { get mode() { return "raw"; }, get raw() { return REQ.body; }, set raw(v) { REQ.body = toStr(v); },
            toString: () => REQ.body,
            update: (v) => { REQ.body = typeof v === "string" ? v : v && v.raw !== undefined ? toStr(v.raw) : JSON.stringify(v); } },
    get name() { return REQ.name; },
    addHeader: (h) => headerList.add(h), removeHeader: (n) => headerList.remove(n), upsertHeader: (h) => headerList.upsert(h),
    addQueryParams: (qs) => { for (const q of [].concat(qs)) queryList.add(typeof q === "string" ? { key: q.split("=")[0], value: q.split("=").slice(1).join("=") } : q); },
    removeQueryParams: (ks) => { for (const k of [].concat(ks)) queryList.remove(k); },
  },
  response: pmRes || undefined,
  info: Object.freeze({ requestName: REQ.name, eventName: PHASE === "pre" ? "prerequest" : "test", iteration: 0, iterationCount: 1, requestId: REQ.id }),
  test,
  get expect() { return getExpect(); },
  sendRequest: (o, cb) => doSend(o, cb, pmResponse),
};
return pm;
});

define("require", (name) => {
  if (name === "crypto-js") return getCryptoJS();
  if (name === "uuid") return Object.freeze({ v4: () => H.uuid() });
  if (name === "chai") return Object.freeze({ expect: getExpect(), AssertionError });
  throw new Error("require(" + show(String(name)) + ") is not available: watt scripts cannot load modules (built in: 'crypto-js', 'uuid', 'chai')");
});

return { drain: drainOne };
}))JS",
};

const size_t kScriptPreludeChunks = sizeof(kScriptPrelude) / sizeof(kScriptPrelude[0]);

}  // namespace watt::detail
