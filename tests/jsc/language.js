// tests/jsc/language.js (make jsc-test): the language as pages use it.
// Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
let fails = 0;
function check(what, ok) { print(`jsc language: ${what}: ${ok ? "ok" : "FAIL"}`); fails += !ok; }

class Shape { constructor(n) { this.n = n; } area() { return 0; } get name() { return this.n; } static make(n) { return new this(n); } }
class Square extends Shape { #side; constructor(s) { super("square"); this.#side = s; } area() { return this.#side ** 2; } }
const sq = Square.make(3);
check("classes, private fields, inheritance", sq.area() === 9 && sq.name === "square" && sq instanceof Shape);

const counter = (() => { let c = 0; return () => ++c; })();
counter(); counter();
const { a, b: [x, , y = 7], ...rest } = { a: 1, b: [2, 3], c: 4, d: 5 };
check("closures, destructuring, rest and spread", counter() === 3 && a === 1 && x === 2 && y === 7 &&
      Object.keys(rest).join() === "c,d" && Math.max(...[1, 5, 2]) === 5);

function* fib() { let [p, q] = [0, 1]; for (;;) { yield p; [p, q] = [q, p + q]; } }
const f10 = []; for (const v of fib()) { if (f10.length === 10) break; f10.push(v); }
check("generators", f10.join() === "0,1,1,2,3,5,8,13,21,34");

const m = new Map([[1, "one"]]), s = new Set([1, 2, 2, 3]), wm = new WeakMap(), key = {};
wm.set(key, 42);
check("Map, Set, WeakMap", m.get(1) === "one" && s.size === 3 && wm.get(key) === 42);

const p = new Proxy({}, { get: (t, k) => k === "hello" ? "world" : Reflect.get(t, k) });
const sym = Symbol("s"), o = { [sym]: 1, a: { b: null } };
check("Proxy, Reflect, Symbol, optional chaining", p.hello === "world" && o[sym] === 1 && o.a?.b?.c === undefined && (o.z ?? 5) === 5);

check("BigInt", (2n ** 100n).toString() === "1267650600228229401496703205376" && BigInt.asUintN(8, 257n) === 1n);

const j = JSON.parse('{"x":[1,2,{"y":"z"}],"u":"\\u00e9"}');
check("JSON", j.x[2].y === "z" && j.u === "é" && JSON.stringify({ a: [1, "b"] }) === '{"a":[1,"b"]}');

const d = new Date(Date.UTC(2026, 9, 17, 12, 30));
check("Date", d.toISOString() === "2026-10-17T12:30:00.000Z" && d.getUTCDay() === 6 && Date.now() > 1.7e12);

let caught = "";
try { null.x; } catch (e) { caught = e.constructor.name; } finally { caught += "!"; }
check("exceptions", caught === "TypeError!");

check("strings", "abc".padStart(5, "-") === "--abc" && "a-b-c".replaceAll("-", "+") === "a+b+c" &&
      "x".repeat(3) === "xxx" && "Straße".toUpperCase() === "STRASSE" && [..."héllo"].length === 5);

check("arrays", [3, 1, 2].toSorted().join() === "1,2,3" && [1, [2, [3]]].flat(2).join() === "1,2,3" &&
      [1, 2, 3].findLast(v => v < 3) === 2 && Array.from({ length: 3 }, (_, i) => i * 2).at(-1) === 4);

const log = [];
async function step(n) { await null; log.push(n); return n * 2; }
Promise.all([step(1), step(2)]).then(r => log.push(r.join()));
(async () => { for await (const v of [Promise.resolve("a"), "b"]) log.push(v); })();
let ticks = 0;
setTimeout(() => {
    ticks++;
    check("promises, async, await; timers", log.join() === "1,2,a,2,4,b" && ticks === 1);
    print(`jsc language: ${fails ? "FAIL" : "ok"}`);
}, 10);
