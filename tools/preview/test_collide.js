// node tools/preview/test_collide.js
// Walk-mode physics against the real block pack (tools/build_preview_pack.py must have been run).
'use strict';
const fs = require('fs');
const path = require('path');
globalThis.window = globalThis;
require('./mesher.js');
require('./collide.js');
const packPath = path.join(__dirname, 'pack.js');
if (!fs.existsSync(packPath)) { console.log('pack.js is missing - run tools/build_preview_pack.py first'); process.exit(2); }
new Function(fs.readFileSync(packPath, 'utf8'))();
const P = window.HYPACK;
const C = window.HYCOL;

let pass = 0, fail = 0;
const ok = (c, what) => { if (c) pass++; else { fail++; console.log('FAIL:', what); } };
const near = (a, b, e = 0.02) => Math.abs(a - b) <= e;

function world(blocks, opts) {
  const p = window.HYMESH.prepare({ blocks }, P);
  return window.HYCOL.create(p, P, opts);
}
const cube = (x, y, z, name) => ({ x, y, z, name: name || 'Rock_Stone_Brick' });
const player = (x, y, z) => ({ x, y, z, vy: 0, onGround: false, crouch: false });
function run(col, p, seconds, inp) { for (let t = 0; t < seconds; t += 1 / 60) C.step(col, p, 1 / 60, inp); }
const still = { mx: 0, mz: 0, jump: false, crouch: false };

// ---- a cube is one box, the ground is a plane ------------------------------------------------------------
{
  const col = world([cube(0, 0, 0)]);
  ok(col.cellBoxes(0, 0, 0).length === 1 && col.cellBoxes(1, 0, 0).length === 0, 'a cube is one box and empty cells have none');
  const p = player(0.5, 5, 0.5);
  run(col, p, 2, still);
  ok(near(p.y, 1) && p.onGround, 'dropped onto a cube, the player stands on its top (y ' + p.y.toFixed(3) + ')');
  const q = player(5.5, 5, 5.5);
  run(col, q, 2, still);
  ok(near(q.y, 0) && q.onGround, 'away from the build the ground plane holds the player at y 0');
  const fall = world([cube(0, 0, 0)], { floor: null });
  const r = player(5.5, 5, 5.5);
  run(fall, r, 1, still);
  ok(r.y < 0 && !r.onGround, 'with no ground plane the player falls');
}

// ---- walls stop you, small steps do not -------------------------------------------------------------------
{
  const wall = []; for (let y = 0; y < 3; y++) for (let z = -3; z <= 3; z++) wall.push(cube(3, y, z));
  const col = world(wall);
  const p = player(0.5, 0, 0.5);
  run(col, p, 3, { mx: 4, mz: 0, jump: false, crouch: false });
  ok(near(p.x, 3 - C.HALF_W, 0.02), 'walking at a wall stops with the body against it (x ' + p.x.toFixed(3) + ')');
  ok(near(p.z, 0.5, 0.001), 'and does not slide sideways');
}
{
  // a half block (slab) is stepped onto automatically, a whole block is not
  const col = world([cube(3, 0, 0, 'Rock_Stone_Brick_Half'), cube(3, 0, 1, 'Rock_Stone_Brick'), cube(4, 0, 0, 'Rock_Stone_Brick_Half'),
                     cube(4, 0, 1, 'Rock_Stone_Brick'), cube(5, 0, 1, 'Rock_Stone_Brick'), cube(6, 0, 1, 'Rock_Stone_Brick')]);
  const p = player(1.5, 0, 0.5);
  run(col, p, 1.1, { mx: 3, mz: 0, jump: false, crouch: false });
  ok(p.x > 4.3 && near(p.y, 0.5, 0.05), 'the player walks up onto half blocks without jumping (x ' + p.x.toFixed(2) + ', y ' + p.y.toFixed(2) + ')');
  const q = player(1.5, 0, 1.5);
  run(col, q, 2, { mx: 3, mz: 0, jump: false, crouch: false });
  ok(q.x < 3 - C.HALF_W + 0.05 && near(q.y, 0), 'but a whole block in the way stops a walker (x ' + q.x.toFixed(2) + ')');
  const j = player(1.5, 0, 1.5);
  let jumped = false;
  for (let t = 0; t < 1.0; t += 1 / 60) { C.step(col, j, 1 / 60, { mx: 3, mz: 0, jump: t < 0.4, crouch: false }); if (j.y > 1) jumped = true; }
  ok(jumped && j.x > 3.6 && near(j.y, 1, 0.05), 'a jump gets onto a whole block (x ' + j.x.toFixed(2) + ', y ' + j.y.toFixed(2) + ')');
}

// ---- stairs are a staircase ---------------------------------------------------------------------------------
{
  // stairs with the tall side toward +x need rotation 3 (n,w,s,e = 0,1,2,3): a row rising to the east
  const blocks = [];
  for (let i = 0; i < 4; i++) { blocks.push({ x: 2 + i, y: i, z: 0, name: 'Rock_Stone_Brick_Stairs', rotation: 3 }); for (let y = 0; y < i; y++) blocks.push(cube(2 + i, y, 0)); }
  const col = world(blocks);
  const p = player(0.5, 0, 0.5);
  let peak = 0;
  for (let t = 0; t < 3; t += 1 / 60) { C.step(col, p, 1 / 60, { mx: 3, mz: 0, jump: false, crouch: false }); peak = Math.max(peak, p.y); }
  ok(peak > 3, 'walking east climbs a flight of stairs with no jumping (highest y ' + peak.toFixed(2) + ')');
}

// ---- ceilings -------------------------------------------------------------------------------------------------
{
  const col = world([cube(0, 3, 0), cube(1, 3, 0), cube(0, 3, 1), cube(1, 3, 1)]);
  const p = player(0.5, 0, 0.5);
  let top = 0;
  for (let t = 0; t < 1.5; t += 1 / 60) { C.step(col, p, 1 / 60, { mx: 0, mz: 0, jump: true, crouch: false }); top = Math.max(top, p.y + C.HEIGHT); }
  ok(top <= 3.001, 'a jump under a low ceiling stops at the ceiling (head reached ' + top.toFixed(3) + ')');
}

// ---- things you walk through ---------------------------------------------------------------------------------
{
  for (const n of ['Kart_Painted_Line', 'Plant_Flower_Common_Pink']) {
    if (P.blocks[n] === undefined) continue;
    const col = world([cube(3, 0, 0, n)]);
    ok(col.cellBoxes(3, 0, 0).length === 0, n + ' has no collision (a painted tile or a flower)');
  }
  if (P.blocks['Kart_Asphalt'] !== undefined) {
    const col = world([cube(0, 0, 0, 'Kart_Asphalt')]);
    ok(col.cellBoxes(0, 0, 0).length === 1, 'asphalt is solid');
  }
}

// ---- hidden materials do not block -------------------------------------------------------------------------------
{
  const hidden = new Set(['Rock_Stone_Brick']);
  const col = world([cube(3, 0, 0)], { hidden });
  ok(col.cellBoxes(3, 0, 0).length === 0, 'a hidden material can be walked through');
}

// ---- rotated models: a stair turned a quarter keeps a full-height side on the turned side -------------------------
{
  const col = world([{ x: 0, y: 0, z: 0, name: 'Rock_Stone_Brick_Stairs', rotation: 0 }, { x: 5, y: 0, z: 0, name: 'Rock_Stone_Brick_Stairs', rotation: 3 }]);
  const tallAt = (boxes, side) => Math.max(...boxes.filter(b => side === 'n' ? b[2] < 0.3 : side === 's' ? b[2] > 0.7 : side === 'e' ? b[3] > 0.7 + 5 : b[0] < 0.3 + 5).map(b => b[4]));
  const b0 = col.cellBoxes(0, 0, 0), b3 = col.cellBoxes(5, 0, 0);
  ok(near(tallAt(b0, 'n'), 1, 0.01) && tallAt(b0, 's') < 0.6, 'stairs at rotation 0 are tall on the north side');
  ok(near(tallAt(b3, 'e'), 1, 0.01) && tallAt(b3, 'w') < 0.6, 'and at rotation 3 on the east side');
}

// ---- flying ------------------------------------------------------------------------------------------------------
{
  const col = world([cube(3, 0, 0), cube(3, 1, 0), cube(3, 2, 0)]);
  const p = player(0.5, 5, 0.5);
  for (let t = 0; t < 2; t += 1 / 60) C.fly(col, p, 1 / 60, { mx: 0, my: 2, mz: 0 });
  ok(p.y > 8, 'flying up leaves the ground behind (y ' + p.y.toFixed(1) + ')');
  const q = player(0.5, 1, 0.5);
  for (let t = 0; t < 2; t += 1 / 60) C.fly(col, q, 1 / 60, { mx: 5, my: 0, mz: 0 });
  ok(q.x < 3 - C.HALF_W + 0.01, 'flying into a wall stops at it');
  const n = player(0.5, 1, 0.5);
  for (let t = 0; t < 2; t += 1 / 60) C.fly(col, n, 1 / 60, { mx: 5, my: 0, mz: 0, noclip: true });
  ok(n.x > 8, 'noclip flies straight through');
}

console.log('\n%d passed, %d failed', pass, fail);
process.exit(fail ? 1 : 0);
