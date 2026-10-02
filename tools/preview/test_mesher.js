// node tools/preview/test_mesher.js [path/to/test_house.json]
// Checks the meshing rules against the real pack (tools/build_preview_pack.py must have been run).
'use strict';
const fs = require('fs');
const path = require('path');
globalThis.window = globalThis;
require('./mesher.js');
const packPath = path.join(__dirname, 'pack.js');
if (!fs.existsSync(packPath)) { console.log('pack.js is missing - run tools/build_preview_pack.py first'); process.exit(2); }
// pack.js is `window.HYPACK=...;window.HYPACK.atlas="data:..."`: evaluate it as a script
new Function(fs.readFileSync(packPath, 'utf8'))();
const P = window.HYPACK;

let pass = 0, fail = 0;
const ok = (c, what) => { if (c) pass++; else { fail++; console.log('FAIL:', what); } };
const near = (a, b, e = 1e-3) => Math.abs(a - b) <= e;

function meshOf(blocks, hidden) {
  const p = window.HYMESH.prepare({ blocks }, P);
  return window.HYMESH.build(p, P, hidden);
}
function verts(m) {
  const out = [];
  for (const s of [m.opaque, m.blend]) for (let k = 0; k < s.v.length; k += 6) out.push({ x: s.v[k], y: s.v[k + 1], z: s.v[k + 2], u: s.v[k + 3], v: s.v[k + 4], s: s.v[k + 5] });
  return out;
}
const one = (name, rot) => verts(meshOf([{ x: 0, y: 0, z: 0, name, rotation: rot }]));
const range = (vs, k) => [Math.min(...vs.map(v => v[k])), Math.max(...vs.map(v => v[k]))];

// ---- the pack knows the blocks -------------------------------------------------------------
for (const n of ['Rock_Stone_Brick', 'Rock_Stone_Brick_Stairs', 'Rock_Stone_Brick_Half', 'Wood_Softwood_Roof', 'Wood_Softwood_Beam', 'Furniture_Village_Door',
                 'Furniture_Village_Window', 'Soil_Grass', 'Plant_Leaves_Oak', 'Wood_Oak_Trunk',
                 '*Wood_Softwood_Roof_State_Definitions_Corner_Left', '*Wood_Softwood_Roof_Hollow_State_Definitions_Inverted_Corner_Right'])
  ok(P.blocks[n] !== undefined, 'pack has ' + n);

// ---- culling -------------------------------------------------------------------------------
const cubeAt = (x, y, z) => ({ x, y, z, name: 'Rock_Stone_Brick' });
ok(meshOf([cubeAt(0, 0, 0)]).faces === 6, 'a lone cube has 6 faces');
ok(meshOf([cubeAt(0, 0, 0), cubeAt(1, 0, 0)]).faces === 10, 'two touching cubes hide the shared faces (10 left)');
const big = []; for (let x = 0; x < 3; x++) for (let y = 0; y < 3; y++) for (let z = 0; z < 3; z++) big.push(cubeAt(x, y, z));
ok(meshOf(big).faces === 54, 'a solid 3x3x3 has only its 54 outside faces');
ok(meshOf(big, new Set(['Rock_Stone_Brick'])).faces === 0, 'hiding a material removes it');

// ---- rotation: stairs and roofs (tall side N, W, S, E for 0..3; upside down 8..11) ----------------
const tallSide = (name, rot) => {
  const vs = one(name, rot).filter(v => v.y > 0.99);
  return [vs.reduce((a, v) => a + v.x, 0) / vs.length, vs.reduce((a, v) => a + v.z, 0) / vs.length];
};
for (const name of ['Rock_Stone_Brick_Stairs', 'Wood_Softwood_Roof_Hollow']) {
  const [x0, z0] = tallSide(name, 0); ok(z0 < 0.4 && near(x0, 0.5, 0.08), name + ' rot 0: tall side north (-Z) got z=' + z0.toFixed(2));
  const [x1, z1] = tallSide(name, 1); ok(x1 < 0.4 && near(z1, 0.5, 0.08), name + ' rot 1: tall side west (-X) got x=' + x1.toFixed(2));
  const [x2, z2] = tallSide(name, 2); ok(z2 > 0.6 && near(x2, 0.5, 0.08), name + ' rot 2: tall side south (+Z) got z=' + z2.toFixed(2));
  const [x3, z3] = tallSide(name, 3); ok(x3 > 0.6 && near(z3, 0.5, 0.08), name + ' rot 3: tall side east (+X) got x=' + x3.toFixed(2));
}
{   // upside down: the full-height part hangs from the top, so vertices at y=0 are the tall side; 8 -> south
  const vs = one('Rock_Stone_Brick_Stairs', 8).filter(v => v.y < 0.01);
  const z = vs.reduce((a, v) => a + v.z, 0) / vs.length;
  ok(z > 0.6, 'stairs rot 8 (upside down): tall side south, got z=' + z.toFixed(2));
  const top = one('Rock_Stone_Brick_Stairs', 8).filter(v => v.y > 0.99);
  ok(top.length > 0, 'upside-down stairs still reach the top of the block');
}

// ---- beams: 0 upright, 4 along Z, 5 along X ---------------------------------------------------
{
  const e = (rot) => { const vs = one('Wood_Softwood_Beam', rot); return [range(vs, 'x'), range(vs, 'y'), range(vs, 'z')].map(r => r[1] - r[0]); };
  const [ux, uy, uz] = e(0), [zx, zy, zz] = e(4), [xx, xy, xz] = e(5);
  ok(uy > 0.95 && ux < 0.6 && uz < 0.6, 'beam rot 0 is upright');
  ok(zz > 0.95 && zx < 0.6 && zy < 0.6, 'beam rot 4 runs along Z');
  ok(xx > 0.95 && xz < 0.6 && xy < 0.6, 'beam rot 5 runs along X');
}

// ---- half blocks: bottom slab, and 8 is the top slab ---------------------------------------------
{
  const lo = range(one('Rock_Stone_Brick_Half', 0), 'y'), hi = range(one('Rock_Stone_Brick_Half', 8), 'y');
  ok(lo[1] <= 0.51 && lo[0] >= -0.01, 'half block rot 0 fills the bottom half');
  ok(hi[0] >= 0.49 && hi[1] <= 1.01, 'half block rot 8 fills the top half');
}

// ---- windows and doors turn with an even/odd rule --------------------------------------------------
{
  const w = (rot) => { const vs = one('Furniture_Village_Window', rot); return [range(vs, 'x'), range(vs, 'z')].map(r => r[1] - r[0]); };
  const [x0, z0] = w(0), [x1, z1] = w(1);
  ok(x0 > z0 * 2, 'window rot 0 is wide along X (a wall running east-west)');
  ok(z1 > x1 * 2, 'window rot 1 is wide along Z (a wall running north-south)');
}

// ---- unknown block -> pink placeholder -----------------------------------------------------------
{
  const p = window.HYMESH.prepare({ blocks: [{ x: 0, y: 0, z: 0, name: 'Definitely_Not_A_Block' }] }, P);
  ok(p.unknown.size === 1, 'an unknown block is reported');
  ok(window.HYMESH.build(p, P).faces === 6, 'and drawn as a cube');
}

// ---- sanity of every face the pack can make ------------------------------------------------------------
{
  let bad = 0, total = 0;
  for (const name of Object.keys(P.blocks)) {
    const m = meshOf([{ x: 0, y: 0, z: 0, name }]);
    for (const v of verts(m)) {
      total++;
      if (!(v.x > -6 && v.x < 6 && v.y > -6 && v.y < 6 && v.z > -6 && v.z < 6) || !(v.u >= 0 && v.u <= 1 && v.v >= 0 && v.v <= 1) || !isFinite(v.s)) bad++;
    }
  }
  ok(bad === 0, 'every vertex of every block is finite, near its block, and inside the atlas (' + bad + ' bad of ' + total + ')');
}

// ---- the race server's custom blocks (tools/race_blocks.json) -----------------------------------------------
if (P.blocks['Kart_Asphalt'] === undefined) console.log('(no race blocks in the pack - skipped)');
else {
  const cat = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'race_blocks.json'), 'utf8'));
  const missing = [];
  for (const b of cat.blocks) {
    if (P.blocks[b.name] === undefined) missing.push(b.name);
    for (const st of Object.keys(b.states || {})) if (P.blocks['*' + b.name + '_State_Definitions_' + st] === undefined) missing.push(b.name + ':' + st);
  }
  ok(missing.length === 0, 'the pack knows every race block (' + missing.join(',') + ')');

  // a plain-block model is baked into a real cube: neighbours hide its faces
  const asphaltAt = (x, y, z) => ({ x, y, z, name: 'Kart_Asphalt' });
  const road3 = []; for (let x = 0; x < 3; x++) for (let y = 0; y < 3; y++) for (let z = 0; z < 3; z++) road3.push(asphaltAt(x, y, z));
  ok(meshOf(road3).faces === 54, 'asphalt blocks hide each other like cubes (54 faces on a 3x3x3)');
  ok(meshOf([{ x: 0, y: 0, z: 0, name: 'Kart_Asphalt' }, { x: 0, y: 0, z: 0, name: 'Rock_Stone_Brick' }]).faces <= 12, 'asphalt next to stone still works');

  // heights and footprints of the models
  const ext = (name, rot) => { const v = one(name, rot); return { x: range(v, 'x'), y: range(v, 'y'), z: range(v, 'z') }; };
  ok(near(ext('Kart_Asphalt_Slab', 0).y[1], 0.5, 0.02), 'the asphalt slab is half a block high');
  ok(near(ext('Kart_Podium_1', 0).y[1], 0.75, 0.02) && near(ext('Kart_Podium_2', 0).y[1], 0.5, 0.02) && near(ext('Kart_Podium_3', 0).y[1], 0.31, 0.02), 'podium steps are 0.75, 0.5 and 0.31 high');
  ok(near(ext('Kart_Painted_Line', 0).y[1], 0.031, 0.01) && near(ext('Kart_Painted_Line', 0).y[0], 0.031, 0.01), 'a painted tile is a flat sheet just above the floor');
  ok(ext('Flag', 0).y[1] > 3.4 && ext('Flag', 0).x[1] > 1.8, 'the flag is 3.5 high and streams east over the next block');
  ok(ext('Red_Workwall', 0).x[0] < -0.9 && ext('Red_Workwall', 0).x[1] > 1.9, 'the work wall is 3 blocks wide, centred on its block');

  // rails lie east-west at rot 0 and north-south at rot 1
  const a0 = ext('Kart_Armco', 0), a1 = ext('Kart_Armco', 1);
  ok(a0.x[1] - a0.x[0] > 0.95 && a0.z[1] - a0.z[0] < 0.2, 'a guard-rail at rot 0 runs east-west');
  ok(a1.z[1] - a1.z[0] > 0.95 && a1.x[1] - a1.x[0] < 0.2, 'and at rot 1 north-south');
  // the corner piece: arms east and south at rot 0, north and east at rot 1
  const c0 = ext('Kart_Armco_Corner', 0), c1 = ext('Kart_Armco_Corner', 1);
  ok(near(c0.x[0], 0.44, 0.03) && near(c0.x[1], 1, 0.03) && near(c0.z[0], 0.44, 0.03) && near(c0.z[1], 1, 0.03), 'the rail corner at rot 0 reaches east and south');
  ok(near(c1.x[0], 0.44, 0.03) && near(c1.x[1], 1, 0.03) && near(c1.z[0], 0, 0.03) && near(c1.z[1], 0.56, 0.03), 'and east and north at rot 1');

  // the roof slab rises to the north at rot 0 and its corner variants exist as states
  const rf = one('Kart_Slate_Roof_Shallow', 0);
  const north = rf.filter(v => v.z < -0.9), south = rf.filter(v => v.z > 0.9);
  ok(Math.max(...north.map(v => v.y)) > 0.95 && Math.max(...south.map(v => v.y)) < 0.1, 'the shallow roof is high on its north end and low on its south end');
}

// ---- NoCube's neon blocks (tools/server_mods): unlit cubes --------------------------------------------------
if (P.blocks['NoCube_Neon_Block_Blue'] === undefined) console.log('(no neon blocks in the pack - skipped)');
else {
  const neon = (x, y, z, n) => ({ x, y, z, name: n || 'NoCube_Neon_Block_Blue' });
  ok(meshOf([neon(0, 0, 0), neon(1, 0, 0)]).faces === 10, 'neon blocks are real cubes: a pair shows 10 faces');
  const nv = one('NoCube_Neon_Block_Red', 0);
  const shades = new Set(nv.map(v => v.s.toFixed(2)));
  ok(nv.length === 24 && Math.min(...nv.map(v => v.s)) > 0.8, 'an unlit neon block is barely shaded (' + [...shades].join(',') + ')');
  const stone = one('Rock_Stone_Brick', 0);
  ok(Math.min(...stone.map(v => v.s)) < 0.6, 'while an ordinary block keeps the full shading');
  const names = ['Black', 'Blue', 'Crimson', 'Cyan', 'Dark_Gray', 'Fire_Red', 'Green', 'Light_Blue', 'Light_Gray', 'Lime', 'Magenta', 'Orange', 'Pink', 'Purple', 'Red', 'White', 'Yellow'];
  ok(names.every(n => P.blocks['NoCube_Neon_Block_' + n] !== undefined), 'all 17 neon colours are in the pack');
}

// ---- soft corner shading (ambient occlusion) and glow --------------------------------------------------------
{
  const total = (blocks, opts) => { const m = window.HYMESH.build(window.HYMESH.prepare({ blocks }, P), P, undefined, opts); return verts(m).reduce((a, v) => a + v.s, 0); };
  ok(near(total([cubeAt(0, 0, 0)]), total([cubeAt(0, 0, 0)], { ao: false }), 1e-6), 'a lone cube has no corners to darken');
  const corner = [cubeAt(0, 0, 0), cubeAt(1, 1, 0), cubeAt(1, 1, 1), cubeAt(0, 0, 1)];
  ok(total(corner) < total(corner, { ao: false }) - 0.2, 'a corner tucked against a wall is darker than with ao off');
  ok(meshOf(corner).faces === window.HYMESH.build(window.HYMESH.prepare({ blocks: corner }, P), P, undefined, { ao: false }).faces, 'ao never changes how many faces there are');
  const vs = verts(meshOf(corner));
  ok(vs.every(v => v.s > 0 && v.s <= 1.0001), 'and never lights a face above full or drops it to black');
  if (P.blocks['NoCube_Neon_Block_Red'] !== undefined) {
    const nv = one('NoCube_Neon_Block_Red', 0);
    ok(nv.every(v => v.s >= 2), 'light-giving faces are flagged (shade 2 + 0..1)');
    ok(one('Rock_Stone_Brick', 0).every(v => v.s < 1.5), 'ordinary faces are not');
  }
}

// ---- a real prefab from the game --------------------------------------------------------------------------
const housePath = process.argv[2] || path.join(__dirname, '..', '..', 'x64', 'Release', 'prefabs', 'test_house.json');
if (fs.existsSync(housePath)) {
  const data = JSON.parse(fs.readFileSync(housePath, 'utf8'));
  const p = window.HYMESH.prepare(data, P);
  const t0 = Date.now();
  const m = window.HYMESH.build(p, P);
  ok(p.unknown.size === 0, 'the real house has no blocks the pack does not know (' + [...p.unknown.keys()].join(',') + ')');
  ok(m.faces > 5000, 'the real house meshes to many faces (' + m.faces + ')');
  console.log('real house: %d blocks, %d faces, %d ms', p.blocks.length, m.faces, Date.now() - t0);
} else console.log('(no real house prefab found - skipped)');

console.log('\n%d passed, %d failed', pass, fail);
process.exit(fail ? 1 : 0);
