// Collision for the viewer's walk mode. Pure logic (no page, no WebGL) so it runs in node too
// (tools/preview/test_collide.js).
//
//   const col = HYCOL.create(prefab, pack, { rotationFor, hidden: Set, floor: 0 | null })
//   HYCOL.step(col, player, dt, { mx, mz, jump, crouch })     walk: gravity, jumping, automatic small steps
//   HYCOL.fly(col, player, dt, { mx, my, mz, noclip })        fly: no gravity, stopped by blocks unless noclip
//
// A player is { x, y, z, vy, onGround, crouch }: x and z are the centre, y the feet. Units are blocks and seconds.
// Every block becomes boxes: a cube is one box; a model (stairs, slabs, fences, furniture) is cut into columns
// on an 8x8 grid, each as tall as the model is in that column, so a stair is a real staircase to walk up.
(function (root) {
  'use strict';

  const EPS = 1e-5;
  const GRID = 8;
  const HALF_W = 0.3, HEIGHT = 1.8, CROUCH_HEIGHT = 1.5, EYE = 1.62, CROUCH_EYE = 1.32;
  const GRAVITY = 28, JUMP = 9.2, MAX_FALL = 55, STEP_UP = 0.6;

  const EMPTY = [];
  const CUBE_BOX = [[0, 0, 0, 1, 1, 1]];

  // The boxes of one block variant, in the block's own 0..1 space (not yet turned), or null when you walk through it.
  function profile(varr, rec, P) {
    if (rec.r === 2 || rec.r === 3) return null;                 // plants and other free-turning scenery
    if (varr.c && varr.m === undefined) return varr.c.some(t => t >= 0) ? CUBE_BOX : null;
    if (varr.m === undefined) return null;
    const polys = P.meshes[varr.m];
    const lo = new Float32Array(GRID * GRID).fill(Infinity), hi = new Float32Array(GRID * GRID).fill(-Infinity);
    let top = 0;
    for (const pr of polys) {
      if (pr[0] & 1) continue;                                   // a flat sheet (cloth, decals, crossed plants)
      const n = pr[4];
      let x0 = 9, x1 = -9, y0 = 9, y1 = -9, z0 = 9, z1 = -9;
      for (let k = 0; k < n; k++) {
        const o = 5 + k * 5, x = pr[o] / P.ps, y = pr[o + 1] / P.ps, z = pr[o + 2] / P.ps;
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; if (z < z0) z0 = z; if (z > z1) z1 = z;
      }
      const cx0 = Math.max(0, Math.floor((x0 + EPS) * GRID)), cx1 = Math.min(GRID - 1, Math.ceil((x1 - EPS) * GRID) - 1);
      const cz0 = Math.max(0, Math.floor((z0 + EPS) * GRID)), cz1 = Math.min(GRID - 1, Math.ceil((z1 - EPS) * GRID) - 1);
      for (let cz = cz0; cz <= Math.max(cz0, cz1); cz++)
        for (let cx = cx0; cx <= Math.max(cx0, cx1); cx++) {
          const i = cz * GRID + cx;
          if (y0 < lo[i]) lo[i] = y0;
          if (y1 > hi[i]) hi[i] = y1;
        }
      if (y1 > top) top = y1;
    }
    if (top < 0.1) return null;                                  // a rug or a painted tile: stepped over
    const boxes = [];
    for (let cz = 0; cz < GRID; cz++) {
      let cx = 0;
      while (cx < GRID) {
        const i = cz * GRID + cx;
        if (!(hi[i] - lo[i] > 0.02)) { cx++; continue; }
        let e = cx + 1;
        while (e < GRID && Math.abs(lo[cz * GRID + e] - lo[i]) < 1e-3 && Math.abs(hi[cz * GRID + e] - hi[i]) < 1e-3) e++;
        boxes.push([cx / GRID, Math.max(0, lo[i]), cz / GRID, e / GRID, hi[i], (cz + 1) / GRID]);
        cx = e;
      }
    }
    // rows with the same span and height are one box
    const merged = [];
    for (const b of boxes) {
      const m = merged.find(q => Math.abs(q[0] - b[0]) < 1e-6 && Math.abs(q[3] - b[3]) < 1e-6 && Math.abs(q[1] - b[1]) < 1e-3 &&
                                 Math.abs(q[4] - b[4]) < 1e-3 && Math.abs(q[5] - b[2]) < 1e-6);
      if (m) m[5] = b[5]; else merged.push(b.slice());
    }
    return merged.length ? merged : null;
  }

  function create(prefab, P, opts) {
    opts = opts || {};
    const rotationFor = opts.rotationFor || root.HYMESH.rotationFor;
    const ck = root.HYMESH.ck, pick = root.HYMESH.pickVariant;
    const profiles = new Map();           // variant object -> boxes | null
    const cache = new Map();              // cell key -> world boxes
    const col = {
      prefab, floor: opts.floor === undefined ? 0 : opts.floor, hidden: opts.hidden || new Set(),
      skip: opts.skip || (() => false),
      clear() { cache.clear(); },
      cellBoxes(x, y, z) {
        const key = ck(x, y, z);
        let r = cache.get(key);
        if (r !== undefined) return r;
        r = EMPTY;
        const idx = prefab.cellIndex.get(key);
        if (idx !== undefined) {
          const b = prefab.blocks[idx];
          if (!col.hidden.has(b.name) && !col.skip(b)) {
            const varr = pick(b.rec, b.x, b.y, b.z);
            let prof = profiles.get(varr);
            if (prof === undefined) { prof = profile(varr, b.rec, P); profiles.set(varr, prof); }
            if (prof) {
              const R = rotationFor(b.rec, b.rot, b.x, b.y, b.z);
              const turned = R[0] === 1 && R[4] === 1 && R[8] === 1;
              r = prof.map(bx => {
                if (turned) return [x + bx[0], y + bx[1], z + bx[2], x + bx[3], y + bx[4], z + bx[5]];
                const lo = [9, 9, 9], hi = [-9, -9, -9];
                for (let c = 0; c < 8; c++) {
                  const px = (c & 1 ? bx[3] : bx[0]) - 0.5, py = (c & 2 ? bx[4] : bx[1]) - 0.5, pz = (c & 4 ? bx[5] : bx[2]) - 0.5;
                  const w = [R[0] * px + R[1] * py + R[2] * pz + 0.5, R[3] * px + R[4] * py + R[5] * pz + 0.5, R[6] * px + R[7] * py + R[8] * pz + 0.5];
                  for (let a = 0; a < 3; a++) { if (w[a] < lo[a]) lo[a] = w[a]; if (w[a] > hi[a]) hi[a] = w[a]; }
                }
                return [x + lo[0], y + lo[1], z + lo[2], x + hi[0], y + hi[1], z + hi[2]];
              });
            }
          }
        }
        cache.set(key, r);
        return r;
      },
      // boxes touching a region
      query(lo, hi) {
        const out = [];
        for (let x = Math.floor(lo[0]); x <= Math.floor(hi[0]); x++)
          for (let y = Math.floor(lo[1]); y <= Math.floor(hi[1]); y++)
            for (let z = Math.floor(lo[2]); z <= Math.floor(hi[2]); z++) {
              const bs = col.cellBoxes(x, y, z);
              for (let i = 0; i < bs.length; i++) out.push(bs[i]);
            }
        return out;
      },
    };
    return col;
  }

  const axisKey = ['x', 'y', 'z'];

  // Moves the player along one axis as far as nothing is in the way. Returns how far it got.
  function moveAxis(col, p, axis, d, h) {
    if (d === 0) return 0;
    const min = [p.x - HALF_W, p.y, p.z - HALF_W], max = [p.x + HALF_W, p.y + h, p.z + HALF_W];
    const lo = min.slice(), hi = max.slice();
    if (d > 0) hi[axis] += d; else lo[axis] += d;
    let move = d;
    for (const bx of col.query(lo, hi)) {
      let overlap = true;
      for (let a = 0; a < 3; a++) if (a !== axis && !(max[a] > bx[a] + EPS && min[a] < bx[a + 3] - EPS)) { overlap = false; break; }
      if (!overlap) continue;
      if (d > 0 && bx[axis] >= max[axis] - EPS) move = Math.min(move, bx[axis] - max[axis]);
      else if (d < 0 && bx[axis + 3] <= min[axis] + EPS) move = Math.max(move, bx[axis + 3] - min[axis]);
    }
    p[axisKey[axis]] += move;
    return move;
  }

  function heightOf(p) { return p.crouch ? CROUCH_HEIGHT : HEIGHT; }

  // One horizontal move, stepping up onto a low ledge (a slab, a stair) when on the ground.
  function moveH(col, p, axis, d, h, canStep) {
    const moved = moveAxis(col, p, axis, d, h);
    if (Math.abs(moved - d) < 1e-6 || !canStep) return moved;
    const save = { x: p.x, y: p.y, z: p.z };
    const lifted = moveAxis(col, p, 1, STEP_UP, h);
    const got = moveAxis(col, p, axis, d, h);
    if (Math.abs(got) > Math.abs(moved) + 1e-4) {
      moveAxis(col, p, 1, -lifted - 0.001, h);                   // settle back down onto the ledge
      return got;
    }
    p.x = save.x; p.y = save.y; p.z = save.z;
    return moved;
  }

  function step(col, p, dt, inp) {
    // a long frame is many short steps, so nothing is tunnelled through
    const n = Math.max(1, Math.ceil(dt / (1 / 120)));
    const d = dt / n;
    for (let s = 0; s < n; s++) {
      p.crouch = !!inp.crouch;
      const h = heightOf(p);
      if (inp.jump && p.onGround) { p.vy = JUMP; p.onGround = false; }
      p.vy = Math.max(-MAX_FALL, p.vy - GRAVITY * d);
      const canStep = p.onGround && p.vy <= 0;
      moveH(col, p, 0, inp.mx * d, h, canStep);
      moveH(col, p, 2, inp.mz * d, h, canStep);
      const dy = p.vy * d;
      const moved = moveAxis(col, p, 1, dy, h);
      if (Math.abs(moved - dy) > 1e-7) { p.onGround = dy < 0; p.vy = 0; }
      else p.onGround = false;
      if (col.floor !== null && p.y <= col.floor) { p.y = col.floor; if (p.vy < 0) p.vy = 0; p.onGround = true; }
      // standing still on a block: the gravity step above moves down a hair and is stopped; keep the flag steady
      if (!p.onGround && p.vy <= 0 && inp.mx === 0 && inp.mz === 0) {
        const probe = { x: p.x, y: p.y, z: p.z };
        if (moveAxis(col, probe, 1, -0.01, h) > -0.0099) p.onGround = true;
      }
    }
  }

  function fly(col, p, dt, inp) {
    const n = Math.max(1, Math.ceil(dt / (1 / 120)));
    const d = dt / n;
    p.vy = 0; p.onGround = false;
    for (let s = 0; s < n; s++) {
      if (inp.noclip) { p.x += inp.mx * d; p.y += inp.my * d; p.z += inp.mz * d; }
      else {
        moveAxis(col, p, 0, inp.mx * d, HEIGHT); moveAxis(col, p, 2, inp.mz * d, HEIGHT); moveAxis(col, p, 1, inp.my * d, HEIGHT);
      }
    }
  }

  root.HYCOL = { create, step, fly, moveAxis, profile, HALF_W, HEIGHT, CROUCH_HEIGHT, EYE, CROUCH_EYE, GRID };
  if (typeof module !== 'undefined') module.exports = root.HYCOL;
})(typeof window !== 'undefined' ? window : globalThis);
