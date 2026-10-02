// Turns a prefab into triangles, using the block pack. Pure logic: no WebGL, no page, so it runs in the
// viewer and in node (tools/preview/test_mesher.js) alike.
//
//   HYMESH.prepare(prefabJson, pack)  -> { blocks, cellIndex, counts, unknown, min, max } | null
//   HYMESH.build(prefab, pack, hiddenNames, opts) -> { opaque: {v, i, n}, blend: {v, i, n}, faces }
//        opts.ao (default true): soft corner shading on cube faces (ambient occlusion)
//
// A vertex is 6 floats: x, y, z, u, v, shade. Positions are in blocks, world = prefab coordinates.
// shade is 0..1 for an ordinary face; a face that gives off light (lamps, neon) is written as 2 + shade, so a
// viewer can keep it bright at night without a second attribute.
(function (root) {
  'use strict';

  const AIR = /^(empty|air|void|none)$/i;

  function hash3(x, y, z, seed) {
    let h = (2166136261 ^ Math.imul(seed | 0, 2654435761)) >>> 0;
    for (const v of [x, y, z]) { h ^= (v | 0) >>> 0; h = Math.imul(h, 16777619) >>> 0; h ^= h >>> 15; }
    h = Math.imul(h, 2246822519) >>> 0; h ^= h >>> 13; h = Math.imul(h, 3266489917) >>> 0; h ^= h >>> 16;
    return (h >>> 0) / 4294967296;
  }

  // A prefab's rotation number: quarter turns about Y (r % 4), applied after quarter turns about X (r >> 2).
  // Read from real exports: stairs and roofs 0..3 have their tall side toward N, W, S, E; 8..11 are upside
  // down; beams 0 upright, 4 along Z, 5 along X.
  function quarter(k, axis) {
    k = ((k % 4) + 4) % 4;
    const c = [1, 0, -1, 0][k], s = [0, 1, 0, -1][k];
    return axis === 'y' ? [c, 0, s, 0, 1, 0, -s, 0, c] : [1, 0, 0, 0, c, -s, 0, s, c];
  }
  function m3mul(a, b) {
    const o = new Array(9);
    for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) o[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    return o;
  }
  function yawMat(rad) { const c = Math.cos(rad), s = Math.sin(rad); return [c, 0, s, 0, 1, 0, -s, 0, c]; }
  const IDENT = [1, 0, 0, 0, 1, 0, 0, 0, 1];

  function rotationFor(rec, rotation, x, y, z) {
    if (rotation >= 0) return m3mul(quarter(rotation & 3, 'y'), quarter((rotation >> 2) & 3, 'x'));
    // No rotation written: blocks the game turns at random get a repeatable random turn.
    const mode = rec.r;
    if (mode === 1) return quarter(Math.floor(hash3(x, y, z, 11) * 4), 'y');
    if (mode === 2) return yawMat(hash3(x, y, z, 12) * Math.PI * 2);
    if (mode === 3) return m3mul(quarter(Math.floor(hash3(x, y, z, 13) * 4), 'y'), quarter(Math.floor(hash3(x, y, z, 14) * 4), 'x'));
    return IDENT;
  }

  // Cube faces: Up, Down, North, South, East, West - normal, then corners TL TR BR BL seen from outside.
  const CUBE = [
    [[0, 1, 0], [[0, 1, 0], [1, 1, 0], [1, 1, 1], [0, 1, 1]]],
    [[0, -1, 0], [[0, 0, 1], [1, 0, 1], [1, 0, 0], [0, 0, 0]]],
    [[0, 0, -1], [[1, 1, 0], [0, 1, 0], [0, 0, 0], [1, 0, 0]]],
    [[0, 0, 1], [[0, 1, 1], [1, 1, 1], [1, 0, 1], [0, 0, 1]]],
    [[1, 0, 0], [[1, 1, 1], [1, 1, 0], [1, 0, 0], [1, 0, 1]]],
    [[-1, 0, 0], [[0, 1, 0], [0, 1, 1], [0, 0, 1], [0, 0, 0]]],
  ];

  // The game's face shading: top 1.0, north/south 0.8, east/west 0.6, bottom 0.5, smoothly in between.
  function shadeOf(nx, ny, nz) {
    const l = Math.hypot(nx, ny, nz) || 1; nx /= l; ny /= l; nz /= l;
    return 0.6 * nx * nx + 0.8 * nz * nz + (ny > 0 ? 1.0 : 0.5) * ny * ny;
  }

  const ck = (x, y, z) => (x + 4096) * 16777216 + (y + 4096) * 4096 + (z + 4096);

  function pickVariant(rec, x, y, z) {
    const vs = rec.v;
    if (vs.length === 1) return vs[0];
    let total = 0;
    for (const v of vs) total += (v.w === undefined ? 1 : v.w);
    let u = hash3(x, y, z, 5) * total;
    for (const v of vs) { u -= (v.w === undefined ? 1 : v.w); if (u < 0) return v; }
    return vs[vs.length - 1];
  }

  function prepare(data, P) {
    const list = data.blocks || [];
    const ax = data.anchorX || 0, ay = data.anchorY || 0, az = data.anchorZ || 0;
    const blocks = [], counts = new Map(), unknown = new Map();
    for (const b of list) {
      if (!b || !b.name || AIR.test(b.name)) continue;
      const idx = P.blocks[b.name];
      const rec = idx === undefined
        ? { o: 0, s: 1, r: 0, v: [{ c: [P.missing, P.missing, P.missing, P.missing, P.missing, P.missing] }] }
        : P.recs[idx];
      if (idx === undefined) unknown.set(b.name, (unknown.get(b.name) || 0) + 1);
      blocks.push({
        x: b.x - ax, y: b.y - ay, z: b.z - az, name: b.name,
        rot: (b.rotation === undefined || b.rotation === null) ? -1 : b.rotation,
        rec, solid: !!(rec.s && rec.o === 0),
      });
      counts.set(b.name, (counts.get(b.name) || 0) + 1);
    }
    if (!blocks.length) return null;
    const cellIndex = new Map();
    const min = [1e9, 1e9, 1e9], max = [-1e9, -1e9, -1e9];
    blocks.forEach((b, i) => {
      cellIndex.set(ck(b.x, b.y, b.z), i);
      const p = [b.x, b.y, b.z];
      for (let k = 0; k < 3; k++) { if (p[k] < min[k]) min[k] = p[k]; if (p[k] > max[k]) max[k] = p[k]; }
    });
    return { blocks, cellIndex, counts, unknown, min, max };
  }

  const AO_LEVEL = [0.5, 0.66, 0.83, 1.0];

  function build(prefab, P, hidden, opts) {
    const useAo = !opts || opts.ao !== false;
    const aw = P.aw, ah = P.ah, ps = P.ps, us = P.us;
    const op = { v: [], i: [], n: 0 }, bl = { v: [], i: [], n: 0 };
    // prefab.isSolid lets a caller that meshes one chunk of a huge build answer for the whole build
    const isSolid = prefab.isSolid || ((x, y, z) => {
      const idx = prefab.cellIndex.get(ck(x, y, z));
      return idx !== undefined && prefab.blocks[idx].solid;
    });
    let faces = 0;
    const emit = (target, pts, uvs, shade) => {
      const base = target.n;
      const sh = Array.isArray(shade) ? shade : null;
      for (let k = 0; k < pts.length; k++) target.v.push(pts[k][0], pts[k][1], pts[k][2], uvs[k][0], uvs[k][1], sh ? sh[k] : shade);
      for (let k = 1; k + 1 < pts.length; k++) target.i.push(base, base + k, base + k + 1);
      target.n += pts.length; faces++;
    };
    for (const b of prefab.blocks) {
      if (hidden && hidden.has(b.name)) continue;
      const rec = b.rec;
      const varr = pickVariant(rec, b.x, b.y, b.z);
      const glow = (rec.u || rec.l) ? 2 : 0;
      const R = rotationFor(rec, b.rot, b.x, b.y, b.z);
      const rot = (px, py, pz) => {
        const x = px - 0.5, y = py - 0.5, z = pz - 0.5;
        return [R[0] * x + R[1] * y + R[2] * z + 0.5 + b.x, R[3] * x + R[4] * y + R[5] * z + 0.5 + b.y, R[6] * x + R[7] * y + R[8] * z + 0.5 + b.z];
      };
      const rotN = (nx, ny, nz) => [R[0] * nx + R[1] * ny + R[2] * nz, R[3] * nx + R[4] * ny + R[5] * nz, R[6] * nx + R[7] * ny + R[8] * nz];
      // The cell an axis-aligned face leans on, if that neighbour is a solid cube.
      const leansOnSolid = (n) => {
        const ax = Math.abs(n[0]) > 0.98 ? Math.sign(n[0]) : 0, ay = Math.abs(n[1]) > 0.98 ? Math.sign(n[1]) : 0, az = Math.abs(n[2]) > 0.98 ? Math.sign(n[2]) : 0;
        return (ax || ay || az) ? isSolid(b.x + ax, b.y + ay, b.z + az) : false;
      };
      if (varr.c) {
        for (let f = 0; f < 6; f++) {
          const tid = varr.c[f];
          if (tid < 0) continue;
          const t = P.tex[tid], nrm = rotN(CUBE[f][0][0], CUBE[f][0][1], CUBE[f][0][2]);
          if (leansOnSolid(nrm)) continue;
          let pts = CUBE[f][1].map(c => rot(c[0], c[1], c[2]));
          let uv = [[0, 0], [t[2], 0], [t[2], t[3]], [0, t[3]]].map(q => [(t[0] + q[0]) / aw, (t[1] + q[1]) / ah]);
          let sh = shadeOf(nrm[0], nrm[1], nrm[2]);
          if (rec.u) sh = 1 - 0.35 * (1 - sh);          // unlit neon: only a hint of shading, to keep the shape readable
          let vs = glow + sh;
          if (useAo && !glow) {
            // Ambient occlusion: a corner tucked against solid neighbours is darker. Only for faces that lie
            // on the grid (every cube face does), looked up through the three cells touching that corner.
            const an = Math.abs(nrm[0]) > 0.9 ? 0 : Math.abs(nrm[1]) > 0.9 ? 1 : Math.abs(nrm[2]) > 0.9 ? 2 : -1;
            if (an >= 0) {
              const sg = Math.sign(nrm[an]), o = [b.x, b.y, b.z], a1 = (an + 1) % 3, a2 = (an + 2) % 3;
              const fr = [b.x, b.y, b.z]; fr[an] += sg;
              const lv = pts.map(p => {
                const d1 = p[a1] > o[a1] + 0.5 ? 1 : -1, d2 = p[a2] > o[a2] + 0.5 ? 1 : -1;
                const c1 = fr.slice(), c2 = fr.slice(), c3 = fr.slice();
                c1[a1] += d1; c2[a2] += d2; c3[a1] += d1; c3[a2] += d2;
                const s1 = isSolid(c1[0], c1[1], c1[2]) ? 1 : 0, s2 = isSolid(c2[0], c2[1], c2[2]) ? 1 : 0, s3 = isSolid(c3[0], c3[1], c3[2]) ? 1 : 0;
                return (s1 && s2) ? 0 : 3 - (s1 + s2 + s3);
              });
              let k = [0, 1, 2, 3];
              if (lv[0] + lv[2] < lv[1] + lv[3]) k = [1, 2, 3, 0];       // split the quad along its lighter diagonal
              pts = k.map(i => pts[i]); uv = k.map(i => uv[i]); vs = k.map(i => sh * AO_LEVEL[lv[i]]);
            }
          }
          emit(t[4] === 2 ? bl : op, pts, uv, vs);
        }
      }
      if (varr.m !== undefined) {
        const t = P.tex[varr.t], polys = P.meshes[varr.m];
        const target = t[4] === 2 ? bl : op;
        const origin = [b.x, b.y, b.z];
        for (const pr of polys) {
          const flags = pr[0], n = pr[4];
          const nrm = rotN(pr[1] / 100, pr[2] / 100, pr[3] / 100);
          const pts = [], uv = [];
          for (let k = 0; k < n; k++) {
            const o = 5 + k * 5;
            pts.push(rot(pr[o] / ps, pr[o + 1] / ps, pr[o + 2] / ps));
            uv.push([(t[0] + pr[o + 3] / us) / aw, (t[1] + pr[o + 4] / us) / ah]);
          }
          // A face lying on the block's boundary, looking out against a solid neighbour, is never seen.
          let hiddenFace = false;
          for (let ax = 0; ax < 3 && !hiddenFace; ax++) {
            const c0 = pts[0][ax] - origin[ax];
            const onLow = Math.abs(c0) < 1e-3, onHigh = Math.abs(c0 - 1) < 1e-3;
            if ((onLow || onHigh) && pts.every(p => Math.abs(p[ax] - pts[0][ax]) < 1e-3) &&
                Math.abs(nrm[ax]) > 0.9 && (nrm[ax] > 0) === onHigh) {
              const d = [0, 0, 0]; d[ax] = Math.sign(nrm[ax]);
              if (isSolid(b.x + d[0], b.y + d[1], b.z + d[2])) hiddenFace = true;
            }
          }
          if (hiddenFace) continue;
          emit(target, pts, uv, glow + ((flags & 4) ? 1.0 : shadeOf(nrm[0], nrm[1], nrm[2])));
        }
      }
    }
    return { opaque: op, blend: bl, faces };
  }

  root.HYMESH = { prepare, build, rotationFor, quarter, m3mul, shadeOf, hash3, ck, pickVariant, AIR };
})(typeof window !== 'undefined' ? window : globalThis);
