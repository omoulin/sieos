// tests/jsc/engine.js (make jsc-test): what the engine needs of SIEOS:
// hot code through the JIT's tiers (the results the same as the
// interpreter's), regular expressions (YARR), Intl and Unicode (ICU), typed
// arrays and DataView, the garbage collector over much memory, WebAssembly.
// Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
let fails = 0;
function check(what, ok) { print(`jsc engine: ${what}: ${ok ? "ok" : "FAIL"}`); fails += !ok; }

function mix(n) {                       // integers, doubles, objects, calls: hot enough for the DFG
    let h = 0x811c9dc5 | 0, acc = 0.5;
    const o = { k: 1 };
    for (let i = 0; i < n; i++) {
        h = Math.imul(h ^ (i & 0xff), 16777619);
        acc = acc * 1.0000001 + (h & 7) / 3;
        o.k = (o.k + (h >>> 28)) % 1000003;
    }
    return `${h >>> 0}:${acc.toFixed(3)}:${o.k}`;
}
noInline(mix);
const r = mix(20000);                   // first in the interpreter, then in each tier as it gets hot
let same = r === "276277093:23357.182:149569";
for (let i = 0; i < 40; i++) same &&= mix(20000) === r;
check("hot code, the same answer in every tier", same);
const jit = typeof $vm === "undefined" && typeof numberOfDFGCompiles === "function" ? numberOfDFGCompiles(mix) : -1;
print(`jsc engine: (the DFG compiled mix ${jit} time(s))`);

const re = /(?<y>\d{4})-(?<m>\d{2})-(?<d>\d{2})/g, text = "on 2026-10-17 and 2027-01-02";
const dates = [...text.matchAll(re)].map(m => m.groups.y + m.groups.d);
check("regular expressions: named groups, lookbehind, Unicode", dates.join() === "202617,202702" &&
      "price: $42".match(/(?<=\$)\d+/)[0] === "42" && /^\p{Lu}\p{Ll}+$/u.test("Élan") && "a1b22c333".split(/\d+/).length === 4);
let words = 0;
for (let i = 0; i < 2000; i++) words += ("the quick brown fox " + i).replace(/\b(\w)(\w*)/g, (m, a, b) => a.toUpperCase() + b).length;
check("regular expressions, many times", words === 2000 * 20 + 6890);

check("Intl: French dates and numbers", new Intl.DateTimeFormat("fr", { dateStyle: "long", timeZone: "UTC" }).format(new Date(Date.UTC(2026, 9, 17))) === "17 octobre 2026" &&
      new Intl.NumberFormat("fr").format(1234567.5).replace(/\s/g, " ") === "1 234 567,5");
check("Intl: collation, case, normalization", ["été", "eté", "ete"].sort(new Intl.Collator("fr").compare).join() === "ete,eté,été" &&
      "ǅ".toLowerCase() === "ǆ" && "é".normalize("NFC") === "é");
check("Intl: words", [...new Intl.Segmenter("fr", { granularity: "word" }).segment("Bonjour, le monde")].filter(s => s.isWordLike).length === 3);

const buf = new ArrayBuffer(16), dv = new DataView(buf), f64 = new Float64Array(buf), u8 = new Uint8Array(buf);
dv.setUint32(0, 0xdeadbeef, true);
f64[1] = Math.PI;
check("typed arrays, DataView", u8[0] === 0xef && dv.getUint32(0) === 0xefbeadde && f64[1] === Math.PI &&
      new Int16Array([1, -2, 3]).map(v => v * 2).join() === "2,-4,6");

let keep = [], total = 0;
for (let round = 0; round < 20; round++) {           // ~200 MB allocated, a few MB kept
    let junk = [];
    for (let i = 0; i < 20000; i++) junk.push({ i, s: "x" + i, a: new Array(8).fill(i) });
    total += junk.length;
    keep.push(junk[round]);
}
gc();
check("the garbage collector over much memory", total === 400000 && keep[19].a[7] === 19 && keep.length === 20);

if (typeof WebAssembly === "object") {
    // (module (func (export "add") (param i32 i32) (result i32) local.get 0 local.get 1 i32.add))
    const bytes = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 7, 1, 96, 2, 127, 127, 1, 127, 3, 2, 1, 0,
                                  7, 7, 1, 3, 97, 100, 100, 0, 0, 10, 9, 1, 7, 0, 32, 0, 32, 1, 106, 11]);
    const add = new WebAssembly.Instance(new WebAssembly.Module(bytes)).exports.add;
    let s = 0;
    for (let i = 0; i < 100000; i++) s = add(s, i) | 0;
    check("WebAssembly", add(2, 40) === 42 && s === 704982704);
} else print("jsc engine: (no WebAssembly in this build)");

print(`jsc engine: ${fails ? "FAIL" : "ok"}`);
