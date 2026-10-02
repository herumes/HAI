// The logic behind bigview.html, the preview for builds too big for viewer.js (millions of blocks). Pure: no WebGL and no page,
// so node can run it (tools/preview/test_bigcore.js).
//
//   data   = HYBIGCORE.decode(window.HYBIG)             compact base64 -> typed arrays
//   core   = HYBIGCORE.prepare(data, window.HYPACK)     name records, a hash grid of every block, the 32-block chunks
//   mesh   = HYBIGCORE.buildChunk(core, chunkNo, pack)  triangles of ONE chunk (uses HYMESH, neighbours looked up in the whole build)
//   ov     = HYBIGCORE.overview(core, rgbPerName)       top-down colour map + one far-away point per 4-block bucket
//
// Block record in the data: int16 x, y, z, uint16 name id, uint8 rotation (255 = none); 9 bytes, little endian.
(function (root) {
  'use strict';
  const CH = 32, CHS = 5;                     // chunk edge and its log2
  const AIR = /^(empty|air|void|none)$/i;

  function decode(big) {
    const n = big.n, bytes = new Uint8Array(n * 9), s = big.data, STEP = 1 << 22;
    let o = 0;
    for (let i = 0; i < s.length; i += STEP) {
      const bin = atob(s.substr(i, STEP));
      for (let j = 0; j < bin.length; j++) bytes[o++] = bin.charCodeAt(j);
    }
    const x = new Int16Array(n), y = new Int16Array(n), z = new Int16Array(n), nid = new Uint16Array(n), rot = new Int16Array(n);
    for (let i = 0, p = 0; i < n; i++, p += 9) {
      x[i] = (bytes[p] | (bytes[p + 1] << 8)) << 16 >> 16;
      y[i] = (bytes[p + 2] | (bytes[p + 3] << 8)) << 16 >> 16;
      z[i] = (bytes[p + 4] | (bytes[p + 5] << 8)) << 16 >> 16;
      nid[i] = bytes[p + 6] | (bytes[p + 7] << 8);
      rot[i] = bytes[p + 8] === 255 ? -1 : bytes[p + 8];
    }
    return { name: big.name, names: big.names, n, x, y, z, nid, rot };
  }

  // ---- the hash grid: cell -> block index ---------------------------------------------------------
  const cellKey = (x, y, z) => ((x + 4096) * 8192 + (z + 4096)) * 512 + (y + 16);
  function hashOf(k) {
    const lo = k >>> 0, hi = (k / 4294967296) | 0;
    let h = Math.imul(lo ^ Math.imul(hi, 0x9E3779B1), 0x85EBCA6B);
    h ^= h >>> 15; h = Math.imul(h, 0xC2B2AE35); h ^= h >>> 13;
    return h;
  }
  class Grid {
    constructor(n) {
      let cap = 1024; while (cap < n * 2.2) cap <<= 1;
      this.mask = cap - 1; this.keys = new Float64Array(cap).fill(-1); this.vals = new Int32Array(cap);
    }
    set(x, y, z, v) {
      const k = cellKey(x, y, z), m = this.mask, keys = this.keys;
      let h = hashOf(k) & m;
      while (keys[h] !== -1 && keys[h] !== k) h = (h + 1) & m;
      keys[h] = k; this.vals[h] = v;
    }
    get(x, y, z) {
      const k = cellKey(x, y, z), m = this.mask, keys = this.keys;
      let h = hashOf(k) & m;
      for (;;) {
        const q = keys[h];
        if (q === k) return this.vals[h];
        if (q === -1) return -1;
        h = (h + 1) & m;
      }
    }
  }

  function prepare(data, P) {
    const missing = { o: 0, s: 1, r: 0, v: [{ c: [P.missing, P.missing, P.missing, P.missing, P.missing, P.missing] }] };
    const recs = [], solidName = new Uint8Array(data.names.length), unknown = [];
    data.names.forEach((nm, i) => {
      const idx = P.blocks[nm];
      const rec = idx === undefined ? missing : P.recs[idx];
      if (idx === undefined) unknown.push(nm);
      recs.push(rec);
      solidName[i] = (rec.s && rec.o === 0 && !AIR.test(nm)) ? 1 : 0;
    });
    const n = data.n, grid = new Grid(n);
    const min = [1e9, 1e9, 1e9], max = [-1e9, -1e9, -1e9];
    for (let i = 0; i < n; i++) {
      const x = data.x[i], y = data.y[i], z = data.z[i];
      grid.set(x, y, z, i);
      if (x < min[0]) min[0] = x; if (x > max[0]) max[0] = x;
      if (y < min[1]) min[1] = y; if (y > max[1]) max[1] = y;
      if (z < min[2]) min[2] = z; if (z > max[2]) max[2] = z;
    }
    const solidAt = (x, y, z) => { const i = grid.get(x, y, z); return i >= 0 && solidName[data.nid[i]] === 1; };
    // chunks: counting sort of the blocks by chunk number
    const chunkNo = (x, y, z) => ((((x >> CHS) + 128) * 256 + ((z >> CHS) + 128)) * 32) + ((y >> CHS) + 2);
    const cnt = new Int32Array(256 * 256 * 32 + 1);
    const cn = new Int32Array(n);
    for (let i = 0; i < n; i++) { const c = chunkNo(data.x[i], data.y[i], data.z[i]); cn[i] = c; cnt[c + 1]++; }
    for (let i = 1; i < cnt.length; i++) cnt[i] += cnt[i - 1];
    const start = cnt.slice(), order = new Int32Array(n), fill = cnt.slice();
    for (let i = 0; i < n; i++) order[fill[cn[i]]++] = i;
    const chunks = [];
    for (let c = 0; c < 256 * 256 * 32; c++) {
      if (start[c + 1] > start[c]) {
        const cy = (c & 31) - 2, cz = ((c >> 5) & 255) - 128, cx = (c >> 13) - 128;
        chunks.push({ no: c, cx, cy, cz, a: start[c], b: start[c + 1], x0: cx * CH, y0: cy * CH, z0: cz * CH, gl: null, state: 0 });
      }
    }
    return { data, P, recs, solidName, grid, solidAt, order, chunks, min, max, unknown, counts: null };
  }

  // Triangles of one chunk. Blocks buried in solid cubes are skipped; neighbours (also across chunk edges) come from the grid.
  function buildChunk(core, ch, hidden, opts) {
    const { data, recs, solidAt, order } = core, blocks = [];
    for (let k = ch.a; k < ch.b; k++) {
      const i = order[k], nm = data.names[data.nid[i]];
      if (hidden && hidden.has(nm)) continue;
      const x = data.x[i], y = data.y[i], z = data.z[i], rec = recs[data.nid[i]];
      const solid = core.solidName[data.nid[i]] === 1;
      if (solid && rec.v.length && rec.v[0].c && solidAt(x + 1, y, z) && solidAt(x - 1, y, z) && solidAt(x, y + 1, z) && solidAt(x, y - 1, z) &&
          solidAt(x, y, z + 1) && solidAt(x, y, z - 1)) continue;
      blocks.push({ x, y, z, name: nm, rot: data.rot[i], rec, solid });
    }
    return root.HYMESH.build({ blocks, isSolid: solidAt }, core.P, hidden, opts || { ao: true });
  }

  // ---- overview: a top-down colour map and far-away points -----------------------------------------
  function overview(core, rgb, cell) {
    cell = cell || 4;
    const d = core.data, n = d.n, min = core.min, max = core.max;
    const w = Math.ceil((max[0] - min[0] + 1) / cell), h = Math.ceil((max[2] - min[2] + 1) / cell);
    const topY = new Int16Array(w * h).fill(-32768), col = new Uint32Array(w * h);
    const slot = new Map(), px = [], py = [], pz = [], pc = [], ptop = [];
    for (let i = 0; i < n; i++) {
      const x = d.x[i], y = d.y[i], z = d.z[i], nm = d.nid[i];
      const c = rgb[nm * 3] | (rgb[nm * 3 + 1] << 8) | (rgb[nm * 3 + 2] << 16);
      const mi = Math.floor((x - min[0]) / cell) + Math.floor((z - min[2]) / cell) * w;
      if (y > topY[mi]) { topY[mi] = y; col[mi] = c; }
      const bk = ((Math.floor(x / 4) + 1024) * 2048 + (Math.floor(z / 4) + 1024)) * 256 + (Math.floor(y / 4) + 4);
      const s = slot.get(bk);
      if (s === undefined) { slot.set(bk, px.length); px.push(x); py.push(y); pz.push(z); pc.push(c); ptop.push(y); }
      else if (y > ptop[s]) { ptop[s] = y; px[s] = x; py[s] = y; pz[s] = z; pc[s] = c; }
    }
    const np = px.length, pos = new Float32Array(np * 3), pcol = new Uint8Array(np * 4);
    for (let i = 0; i < np; i++) {
      pos[i * 3] = px[i] + 0.5; pos[i * 3 + 1] = py[i] + 0.5; pos[i * 3 + 2] = pz[i] + 0.5;
      pcol[i * 4] = pc[i] & 255; pcol[i * 4 + 1] = (pc[i] >> 8) & 255; pcol[i * 4 + 2] = (pc[i] >> 16) & 255; pcol[i * 4 + 3] = 255;
    }
    return { w, h, cell, topY, col, np, pos, pcol };
  }

  root.HYBIGCORE = { CH, decode, prepare, buildChunk, overview, Grid, cellKey };
})(typeof window !== 'undefined' ? window : globalThis);
