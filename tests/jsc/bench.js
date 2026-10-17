// tests/jsc/bench.js (make jsc-test): a small benchmark, run with the JIT
// and without (--useJIT=0): n-body, string building, objects and closures.
// Prints its time; the answers checked.
// Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
function nbody(steps) {
    const PI = Math.PI, SM = 4 * PI * PI, DPY = 365.24;
    const B = (x, y, z, vx, vy, vz, m) => ({ x, y, z, vx: vx * DPY, vy: vy * DPY, vz: vz * DPY, m: m * SM });
    const bs = [B(0, 0, 0, 0, 0, 0, 1),
        B(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01, 1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05, 9.54791938424326609e-04),
        B(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01, -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05, 2.85885980666130812e-04),
        B(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01, 2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05, 4.36624404335156298e-05),
        B(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01, 2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05, 5.15138902046611451e-05)];
    let px = 0, py = 0, pz = 0;
    for (const b of bs) { px += b.vx * b.m; py += b.vy * b.m; pz += b.vz * b.m; }
    bs[0].vx = -px / SM; bs[0].vy = -py / SM; bs[0].vz = -pz / SM;
    for (let s = 0; s < steps; s++) {
        for (let i = 0; i < bs.length; i++) {
            const a = bs[i];
            for (let j = i + 1; j < bs.length; j++) {
                const b = bs[j], dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                const d2 = dx * dx + dy * dy + dz * dz, mag = 0.01 / (d2 * Math.sqrt(d2));
                a.vx -= dx * b.m * mag; a.vy -= dy * b.m * mag; a.vz -= dz * b.m * mag;
                b.vx += dx * a.m * mag; b.vy += dy * a.m * mag; b.vz += dz * a.m * mag;
            }
        }
        for (const c of bs) { c.x += 0.01 * c.vx; c.y += 0.01 * c.vy; c.z += 0.01 * c.vz; }
    }
    let e = 0;
    for (let i = 0; i < bs.length; i++) {
        const a = bs[i];
        e += 0.5 * a.m * (a.vx * a.vx + a.vy * a.vy + a.vz * a.vz);
        for (let j = i + 1; j < bs.length; j++) { const b = bs[j]; e -= a.m * b.m / Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z); }
    }
    return e.toFixed(9);
}
function strings(n) {
    const parts = [];
    for (let i = 0; i < n; i++) parts.push(`<li class="item-${i % 7}">${String(i * 31 % 1000).padStart(3, "0")}</li>`);
    return parts.join("").length;
}
function objects(n) {
    const make = i => ({ id: i, tag: "t" + (i % 13), score: (i * 2654435761) % 1000, get rank() { return this.score >> 4; } });
    const list = Array.from({ length: n }, (_, i) => make(i));
    const by = new Map();
    for (const o of list) by.set(o.tag, (by.get(o.tag) || 0) + o.rank);
    return [...by.values()].reduce((a, b) => a + b, 0);
}
const t0 = Date.now();
const e = nbody(300000), sl = strings(200000), ob = objects(200000);
const ms = Date.now() - t0;
const ok = e === "-0.169087840" && sl === 5400000 && ob === 6150400;
print(`jsc bench: ${ms} ms (n-body ${e}, strings ${sl}, objects ${ob})`);
print(`jsc bench: ${ok ? "ok" : "FAIL"}`);
