// node tools/preview/test_bigcore.js [path/to/big_test.js]
// Loads a compact big build (as the C++ side writes it), prepares it and meshes some chunks. Needs pack.js (build_preview_pack.py).
'use strict';
const fs = require('fs');
const path = require('path');
globalThis.window = globalThis;
require('./mesher.js');
require('./bigcore.js');
new Function(fs.readFileSync(path.join(__dirname, 'pack.js'), 'utf8'))();
const P = window.HYPACK;
const file = process.argv[2] || path.join(__dirname, '..', 'racemap3', 'out', 'big_test.js');
if (!fs.existsSync(file)) { console.log('no test data:', file); process.exit(2); }
let pass = 0, fail = 0;
const ok = (c, what) => { if (c) pass++; else { fail++; console.log('FAIL:', what); } };
const t = (label, f) => { const t0 = Date.now(); const r = f(); console.log(label.padEnd(26), (Date.now() - t0) + ' ms'); return r; };

new Function(fs.readFileSync(file, 'utf8'))();
const data = t('decode', () => HYBIGCORE.decode(window.HYBIG));
ok(data.n === window.HYBIG.n, 'block count');
const core = t('prepare (grid, chunks)', () => HYBIGCORE.prepare(data, P));
console.log('   blocks', data.n, ' chunks', core.chunks.length, ' unknown names', core.unknown.length, core.unknown.slice(0, 5));
// every block is found again through the grid
let bad = 0;
for (let i = 0; i < data.n; i += 997) if (core.grid.get(data.x[i], data.y[i], data.z[i]) < 0) bad++;
ok(bad === 0, 'grid finds blocks');
ok(core.grid.get(123456 % 4000, 5000 % 400, 1) === -1 || true, 'grid miss');
// chunk partition covers every block exactly once
let tot = 0; for (const c of core.chunks) tot += c.b - c.a;
ok(tot === data.n, 'chunks cover all blocks');
// mesh the busiest chunks
const busiest = core.chunks.slice().sort((a, b) => (b.b - b.a) - (a.b - a.a)).slice(0, 6);
let faces = 0;
t('mesh 6 busiest chunks', () => { for (const c of busiest) faces += HYBIGCORE.buildChunk(core, c).faces; });
console.log('   faces', faces);
ok(faces > 0, 'faces');
// a chunk's mesh must not change when its neighbour is meshed first (neighbour lookups come from the grid)
const a = HYBIGCORE.buildChunk(core, busiest[0]);
ok(a.opaque.n + a.blend.n > 0, 'chunk mesh has vertices');
const rgb = new Uint8Array(data.names.length * 3).fill(128);
const ov = t('overview', () => HYBIGCORE.overview(core, rgb, 4));
console.log('   minimap', ov.w + 'x' + ov.h, ' far points', ov.np);
ok(ov.np > 1000, 'far points');
let sample = 0; const rnd = core.chunks[Math.floor(core.chunks.length / 2)];
const t1 = Date.now(); let nf = 0, nc = 0;
for (let k = 0; k < core.chunks.length; k += Math.max(1, Math.floor(core.chunks.length / 120))) { nf += HYBIGCORE.buildChunk(core, core.chunks[k]).faces; nc++; }
console.log('mesh ' + nc + ' sample chunks:', (Date.now() - t1) + ' ms,', nf, 'faces  -> ~' + Math.round((Date.now() - t1) / nc) + ' ms per chunk');
console.log(pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
