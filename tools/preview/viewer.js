// The prefab viewer: four ways to look at a build (orbit, free fly, walk, layer map), a sky with a time of day,
// slicing, measuring, picking, screenshots. The meshing (mesher.js) and the walking physics (collide.js) are
// separate and tested in node; this file is the page.
'use strict';
const $ = id => document.getElementById(id);
const cv = $('c'), mapCv = $('map'), statusEl = $('status');
let gl, packReady = false, atlasTex, atlasImage, thumbCanvas;
const progs = {};

// ============================================================================================
// settings (remembered between visits, when the browser lets us)
// ============================================================================================
const S = { mode: 'orbit', ortho: false, fov: 60, spin: false, spinSpeed: 12, sky: 'time', time: 14, haze: 30, ground: 'grass',
  ao: true, grid: false, bounds: false, outline: true, flySpeed: 14, sens: 100, flyCollide: false, invY: false, sideHidden: false };
const PERSIST = ['ortho', 'fov', 'spinSpeed', 'sky', 'time', 'haze', 'ground', 'ao', 'grid', 'bounds', 'outline', 'flySpeed', 'sens', 'flyCollide', 'invY', 'sideHidden'];
try { Object.assign(S, JSON.parse(localStorage.getItem('hyai_preview') || '{}')); } catch (e) { /* private window */ }
S.mode = 'orbit'; S.spin = false;
function saveSettings() { try { const o = {}; for (const k of PERSIST) o[k] = S[k]; localStorage.setItem('hyai_preview', JSON.stringify(o)); } catch (e) { /* ignore */ } }

// ============================================================================================
// maths
// ============================================================================================
const M4 = {
  mul(a, b) { const o = new Float32Array(16); for (let i = 0; i < 4; i++) for (let j = 0; j < 4; j++) { let s = 0; for (let k = 0; k < 4; k++) s += a[k * 4 + j] * b[i * 4 + k]; o[i * 4 + j] = s; } return o; },
  persp(fov, asp, n, f) { const t = 1 / Math.tan(fov / 2), o = new Float32Array(16); o[0] = t / asp; o[5] = t; o[10] = (f + n) / (n - f); o[11] = -1; o[14] = 2 * f * n / (n - f); return o; },
  ortho(hw, hh, n, f) { const o = new Float32Array(16); o[0] = 1 / hw; o[5] = 1 / hh; o[10] = -2 / (f - n); o[14] = -(f + n) / (f - n); o[15] = 1; return o; },
  look(e, c, u) {
    let zx = e[0] - c[0], zy = e[1] - c[1], zz = e[2] - c[2], l = Math.hypot(zx, zy, zz); zx /= l; zy /= l; zz /= l;
    let xx = u[1] * zz - u[2] * zy, xy = u[2] * zx - u[0] * zz, xz = u[0] * zy - u[1] * zx; l = Math.hypot(xx, xy, xz); xx /= l; xy /= l; xz /= l;
    const yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
    return new Float32Array([xx, yx, zx, 0, xy, yy, zy, 0, xz, yz, zz, 0, -(xx * e[0] + xy * e[1] + xz * e[2]), -(yx * e[0] + yy * e[1] + yz * e[2]), -(zx * e[0] + zy * e[1] + zz * e[2]), 1]);
  }
};
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
const lerp = (a, b, t) => a + (b - a) * t;
const mix3 = (a, b, t) => [lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)];
const smooth = (a, b, x) => { const t = clamp((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); };
const norm3 = v => { const l = Math.hypot(v[0], v[1], v[2]) || 1; return [v[0] / l, v[1] / l, v[2] / l]; };
const prettyName = n => n.replace(/^\*/, '').replace('_State_Definitions_', ' ~ ');

function toast(msg, ms) {
  const t = $('toast'); t.textContent = msg; t.classList.add('show');
  clearTimeout(toast.h); toast.h = setTimeout(() => t.classList.remove('show'), ms || 2200);
}

// ============================================================================================
// the prefab
// ============================================================================================
let prefab = null, mesh = null, collider = null;
const hidden = new Set();
let counts = new Map();
const ck = HYMESH.ck;
const clipMin = [0, 0, 0], clipMax = [0, 0, 0], fullMin = [0, 0, 0], fullMax = [0, 0, 0];
let selected = null, measure = { on: false, pts: [] }, hover = null;
let dirty = true;
const want = () => { dirty = true; };

function effClip() {
  const lo = clipMin.slice(), hi = clipMax.slice();
  if ($('onlylayer').checked) lo[1] = hi[1];
  return { lo, hi };
}

function buildMesh() {
  const built = HYMESH.build(prefab, window.HYPACK, hidden, { ao: S.ao });
  const up = (t, arr, Type) => { const buf = gl.createBuffer(); gl.bindBuffer(t, buf); gl.bufferData(t, new Type(arr), gl.STATIC_DRAW); return buf; };
  const make = (s) => s.i.length ? { vbo: up(gl.ARRAY_BUFFER, s.v, Float32Array), ibo: up(gl.ELEMENT_ARRAY_BUFFER, s.i, Uint32Array), count: s.i.length } : null;
  if (mesh) for (const k of ['opaque', 'blend']) if (mesh[k]) { gl.deleteBuffer(mesh[k].vbo); gl.deleteBuffer(mesh[k].ibo); }
  mesh = { opaque: make(built.opaque), blend: make(built.blend), faces: built.faces };
  if (collider) collider.clear();
  layerCache.clear();
  want(); updateInfo();
}

function updateInfo() {
  if (!prefab) return;
  const w = fullMax[0] - fullMin[0] + 1, h = fullMax[1] - fullMin[1] + 1, d = fullMax[2] - fullMin[2] + 1;
  let text = prefab.name + '\n' + prefab.blocks.length.toLocaleString() + ' blocks, ' + w + ' wide x ' + h + ' tall x ' + d + ' deep\n' +
    counts.size + ' materials, ' + (mesh ? mesh.faces.toLocaleString() : 0) + ' faces drawn';
  if (prefab.unknown.size) text += '\n' + [...prefab.unknown.values()].reduce((a, b) => a + b, 0) + ' block(s) the pack does not know are shown pink: ' + [...prefab.unknown.keys()].slice(0, 4).join(', ');
  $('info').textContent = text;
}

function loadPrefab(data, label) {
  const p = HYMESH.prepare(data, window.HYPACK);
  if (!p) { $('info').textContent = 'no blocks in that file'; return; }
  prefab = { name: label || 'prefab', blocks: p.blocks, min: p.min, max: p.max, cellIndex: p.cellIndex, unknown: p.unknown };
  counts = p.counts;
  hidden.clear(); selected = null; hover = null; measure.pts = []; $('measureOut').textContent = '';
  for (let a = 0; a < 3; a++) { fullMin[a] = clipMin[a] = p.min[a]; fullMax[a] = clipMax[a] = p.max[a]; }
  setupSliders();
  collider = HYCOL.create(prefab, window.HYPACK, { hidden, floor: S.ground === 'none' ? null : 0, skip: b => /^(Fluid|Water|Lava)/.test(b.name) });
  layerLists = null;
  buildMaterials();
  buildMesh();
  statusEl.style.display = 'none';
  updateSel();
  setView('iso', true);
  if (S.mode === 'walk') enterWalk(); else if (S.mode === 'fly') want();
}

// ---- the slice sliders ---------------------------------------------------------------------
function setupSliders() {
  const ids = [['xmin', 0, 0], ['xmax', 0, 1], ['ymin', 1, 0], ['ymax', 1, 1], ['zmin', 2, 0], ['zmax', 2, 1]];
  for (const [id, a, hi] of ids) {
    const el = $(id); el.min = fullMin[a]; el.max = fullMax[a]; el.step = 1; el.value = hi ? fullMax[a] : fullMin[a];
  }
  updateSliceInfo();
}
function readSliders() {
  const get = id => +$(id).value;
  clipMin[0] = get('xmin'); clipMax[0] = get('xmax'); clipMin[1] = get('ymin'); clipMax[1] = get('ymax'); clipMin[2] = get('zmin'); clipMax[2] = get('zmax');
}
function writeSliders() {
  const set = (id, v) => { $(id).value = v; };
  set('xmin', clipMin[0]); set('xmax', clipMax[0]); set('ymin', clipMin[1]); set('ymax', clipMax[1]); set('zmin', clipMin[2]); set('zmax', clipMax[2]);
}
let sliceTimer = 0;
function updateSliceInfo() {
  cancelAnimationFrame(sliceTimer);
  sliceTimer = requestAnimationFrame(() => {
    if (!prefab) return;
    const { lo, hi } = effClip();
    let n = 0;
    const full = lo.every((v, a) => v === fullMin[a]) && hi.every((v, a) => v === fullMax[a]);
    if (!full) for (const b of prefab.blocks) if (b.x >= lo[0] && b.x <= hi[0] && b.y >= lo[1] && b.y <= hi[1] && b.z >= lo[2] && b.z <= hi[2]) n++;
    $('sliceInfo').textContent = full ? 'showing everything' : 'x ' + lo[0] + '..' + hi[0] + '   y ' + lo[1] + '..' + hi[1] + '   z ' + lo[2] + '..' + hi[2] + '   (' + n.toLocaleString() + ' blocks)';
  });
}
for (const id of ['xmin', 'xmax', 'ymin', 'ymax', 'zmin', 'zmax']) {
  $(id).oninput = () => {
    // the two ends of a range never cross: the one you did not touch is pushed along
    const a = 'xyz'.indexOf(id[0]), isMax = id.endsWith('max');
    const lo = $(id[0] + 'min'), hi = $(id[0] + 'max');
    if (+lo.value > +hi.value) { if (isMax) lo.value = hi.value; else hi.value = lo.value; }
    readSliders(); layerCache.clear(); updateSliceInfo(); want();
  };
}
$('sliceReset').onclick = () => { for (let a = 0; a < 3; a++) { clipMin[a] = fullMin[a]; clipMax[a] = fullMax[a]; } $('onlylayer').checked = false; writeSliders(); layerCache.clear(); updateSliceInfo(); want(); };
$('onlylayer').onchange = () => { layerCache.clear(); updateSliceInfo(); want(); };
function moveLayer(d) {
  if (!prefab) return;
  clipMax[1] = clamp(clipMax[1] + d, fullMin[1], fullMax[1]);
  if (clipMin[1] > clipMax[1]) clipMin[1] = clipMax[1];
  writeSliders(); layerCache.clear(); updateSliceInfo(); want();
  if (S.mode === 'map') toast('layer y = ' + clipMax[1]);
}
$('layerup').onclick = () => moveLayer(1);
$('layerdown').onclick = () => moveLayer(-1);

// ---- materials --------------------------------------------------------------------------------
function tileOf(name) {
  const P = window.HYPACK, idx = P.blocks[name], rec = idx === undefined ? null : P.recs[idx];
  let tid = P.missing;
  if (rec) { const v = rec.v[0]; tid = v.c ? (v.c[0] >= 0 ? v.c[0] : v.c[2]) : v.t; if (tid === undefined || tid < 0) tid = P.missing; }
  return P.tex[tid];
}
function buildMaterials() {
  const P = window.HYPACK, el = $('mats'); el.innerHTML = '';
  if (!thumbCanvas) {
    thumbCanvas = document.createElement('canvas'); thumbCanvas.width = P.aw; thumbCanvas.height = P.ah;
    thumbCanvas.getContext('2d').drawImage(atlasImage, 0, 0);
  }
  const rows = [...counts.entries()].sort((a, b) => b[1] - a[1]);
  for (const [name, n] of rows) {
    const row = document.createElement('div'); row.className = 'm';
    const c = document.createElement('canvas'); c.width = 20; c.height = 20;
    const g = c.getContext('2d'); g.imageSmoothingEnabled = false;
    const t = tileOf(name);
    g.drawImage(thumbCanvas, t[0], t[1], t[2], t[3], 0, 0, 20, 20);
    const label = document.createElement('div'); label.className = 'n'; label.textContent = prettyName(name); label.title = name;
    const cnt = document.createElement('span'); cnt.className = 'c'; cnt.textContent = n.toLocaleString();
    row.append(c, label, cnt); row.dataset.name = name;
    row.onclick = () => toggleMaterial(name);
    row.ondblclick = () => isolateMaterial(name);
    el.appendChild(row);
  }
  applyFilter();
}
function refreshRows() { document.querySelectorAll('#mats div.m').forEach(r => r.classList.toggle('off', hidden.has(r.dataset.name))); }
function materialsChanged() { refreshRows(); buildMesh(); updateSel(); }
function toggleMaterial(name) { if (hidden.has(name)) hidden.delete(name); else hidden.add(name); materialsChanged(); }
function isolateMaterial(name) { hidden.clear(); for (const k of counts.keys()) if (k !== name) hidden.add(k); materialsChanged(); }
function applyFilter() {
  const q = $('filter').value.trim().toLowerCase();
  document.querySelectorAll('#mats div.m').forEach(r => { r.style.display = !q || r.dataset.name.toLowerCase().includes(q) ? '' : 'none'; });
}
$('filter').oninput = applyFilter;
$('showall').onclick = () => { hidden.clear(); if (prefab) materialsChanged(); };

function materialText() {
  const rows = [...counts.entries()].sort((a, b) => b[1] - a[1]);
  let total = 0;
  const lines = rows.map(([n, c]) => { total += c; return c.toLocaleString('en-US') + ' x ' + n + '   (' + Math.floor(c / 100) + ' stacks + ' + (c % 100) + ')'; });
  return (prefab ? prefab.name : 'prefab') + ' - ' + total.toLocaleString('en-US') + ' blocks\n(stacks of 100)\n\n' + lines.join('\n') + '\n';
}
async function copyText(text) {
  try { await navigator.clipboard.writeText(text); return true; } catch (e) { /* fall through */ }
  const ta = document.createElement('textarea'); ta.value = text; ta.style.position = 'fixed'; ta.style.opacity = 0; document.body.appendChild(ta); ta.select();
  let ok = false; try { ok = document.execCommand('copy'); } catch (e) { /* ignore */ }
  ta.remove(); return ok;
}
$('copymats').onclick = async () => { if (prefab) toast(await copyText(materialText()) ? 'Material list copied' : 'Could not copy - use Save list'); };
function download(name, blob) { const a = document.createElement('a'); a.download = name; a.href = URL.createObjectURL(blob); a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 4000); }
$('savemats').onclick = () => { if (prefab) download(prefab.name.replace(/\.json$/i, '') + '_materials.txt', new Blob([materialText()], { type: 'text/plain' })); };

// ============================================================================================
// shaders
// ============================================================================================
const BLOCK_VS = `#version 300 es
in vec3 aPos; in vec2 aUv; in float aShade;
uniform mat4 uVP; out vec2 vUv; out float vShade; out vec3 vWorld;
void main() { vUv = aUv; vShade = aShade; vWorld = aPos; gl_Position = uVP * vec4(aPos, 1.0); }`;
const BLOCK_FS = `#version 300 es
precision highp float;
in vec2 vUv; in float vShade; in vec3 vWorld;
uniform sampler2D uAtlas; uniform vec3 uMin; uniform vec3 uMax; uniform float uBlend;
uniform vec3 uAmb; uniform float uGlow; uniform vec3 uEye; uniform vec3 uFog; uniform float uFogD;
out vec4 o;
void main() {
  if (any(lessThan(vWorld, uMin - 0.0001)) || any(greaterThan(vWorld, uMax + 1.0001))) discard;
  vec4 c = texture(uAtlas, vUv);
  if (uBlend < 0.5 && c.a < 0.5) discard;
  if (uBlend > 0.5 && c.a < 0.02) discard;
  bool em = vShade > 1.5;
  float sh = em ? vShade - 2.0 : vShade;
  vec3 col = em ? c.rgb * (0.85 + 0.15 * sh) * uGlow : c.rgb * sh * uAmb;
  float fd = length(vWorld - uEye) * uFogD;
  col = mix(col, uFog, clamp(1.0 - exp(-fd * fd), 0.0, 1.0));
  o = vec4(col, uBlend > 0.5 ? c.a : 1.0);
}`;
const GROUND_VS = `#version 300 es
in vec3 aPos; uniform mat4 uVP; out vec3 vW;
void main() { vW = aPos; gl_Position = uVP * vec4(aPos, 1.0); }`;
const GROUND_FS = `#version 300 es
precision highp float;
in vec3 vW; uniform sampler2D uAtlas; uniform vec4 uRect; uniform vec2 uAtlasSize; uniform vec3 uAvg; uniform vec3 uAmb;
uniform vec3 uEye; uniform vec3 uFog; uniform float uFogD; uniform float uMode; uniform float uGrid;
out vec4 o;
void main() {
  vec2 uv = vW.xz;
  float dist = length(vW - uEye);
  vec3 col;
  if (uMode < 0.5) {
    vec2 f = fract(uv);
    vec2 px = uRect.xy + floor(f * uRect.zw) + 0.5;
    col = texture(uAtlas, px / uAtlasSize).rgb * 0.92;
    col = mix(col, uAvg, smoothstep(24.0, 100.0, dist));
  } else {
    col = vec3(0.52, 0.54, 0.58) * (mod(floor(uv.x) + floor(uv.y), 2.0) < 0.5 ? 1.0 : 0.94);
  }
  if (uGrid > 0.5) {
    vec2 g = abs(fract(uv - 0.5) - 0.5) / max(fwidth(uv), vec2(1e-5));
    float l = 1.0 - min(min(g.x, g.y), 1.0);
    vec2 g8 = abs(fract(uv / 8.0 - 0.5) - 0.5) / max(fwidth(uv / 8.0), vec2(1e-5));
    float l8 = 1.0 - min(min(g8.x, g8.y), 1.0);
    float fade = 1.0 - smoothstep(24.0, 90.0, dist);
    col = mix(col, vec3(0.04), clamp(l * 0.35 + l8 * 0.35, 0.0, 0.7) * fade);
  }
  col *= uAmb;
  float fd = dist * uFogD;
  o = vec4(mix(col, uFog, clamp(1.0 - exp(-fd * fd), 0.0, 1.0)), 1.0);
}`;
const SKY_VS = `#version 300 es
out vec2 vP;
void main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); vP = p * 2.0 - 1.0; gl_Position = vec4(vP, 1.0, 1.0); }`;
const SKY_FS = `#version 300 es
precision highp float;
in vec2 vP; uniform vec3 uF; uniform vec3 uR; uniform vec3 uU; uniform float uTan; uniform float uAsp; uniform float uOrtho;
uniform vec3 uTop; uniform vec3 uHor; uniform vec3 uSun; uniform float uSunOn; uniform float uStars;
out vec4 o;
void main() {
  vec3 d = normalize(uF + uR * vP.x * uTan * uAsp + uU * vP.y * uTan);
  if (uOrtho > 0.5) d = normalize(uF + uU * vP.y * 0.55);
  float h = d.y;
  vec3 col = mix(uHor, uTop, pow(clamp(h, 0.0, 1.0), 0.5));
  col = mix(col, uHor * 0.9, clamp(-h * 5.0, 0.0, 1.0));
  if (uSunOn > 0.5) {
    float s = max(dot(d, uSun), 0.0);
    col += vec3(1.0, 0.93, 0.75) * (pow(s, 1400.0) * 3.0 + pow(s, 14.0) * 0.16);
    float m = max(dot(d, -uSun), 0.0);
    col += vec3(0.85, 0.9, 1.0) * pow(m, 2500.0) * uStars * 1.3;
  }
  if (uStars > 0.01 && h > 0.0) {
    vec3 q = floor(d * 420.0);
    float n = fract(sin(dot(q, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
    col += vec3(step(0.9975, n)) * uStars * smoothstep(0.0, 0.15, h);
  }
  o = vec4(col, 1.0);
}`;
const LINE_VS = `#version 300 es
in vec3 aPos; uniform mat4 uVP; void main() { gl_Position = uVP * vec4(aPos, 1.0); }`;
const LINE_FS = `#version 300 es
precision highp float; uniform vec4 uColor; out vec4 o; void main() { o = uColor; }`;

function compile(vs, fs) {
  const mk = (t, s) => { const sh = gl.createShader(t); gl.shaderSource(sh, s); gl.compileShader(sh); if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(sh)); return sh; };
  const p = gl.createProgram(); gl.attachShader(p, mk(gl.VERTEX_SHADER, vs)); gl.attachShader(p, mk(gl.FRAGMENT_SHADER, fs)); gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
  const u = {}; const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < n; i++) { const info = gl.getActiveUniform(p, i); u[info.name] = gl.getUniformLocation(p, info.name); }
  return { p, u, a: n => gl.getAttribLocation(p, n) };
}

let groundBuf, lineBuf, groundAvg = [0.4, 0.6, 0.25];
async function initGL() {
  gl = cv.getContext('webgl2', { antialias: true, alpha: true, preserveDrawingBuffer: true, premultipliedAlpha: false });
  if (!gl) throw new Error('This browser has no WebGL2.');
  progs.block = compile(BLOCK_VS, BLOCK_FS); progs.ground = compile(GROUND_VS, GROUND_FS);
  progs.sky = compile(SKY_VS, SKY_FS); progs.line = compile(LINE_VS, LINE_FS);
  atlasImage = new Image(); atlasImage.src = window.HYPACK.atlas; await atlasImage.decode();
  atlasTex = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, atlasTex);
  gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, atlasImage);
  gl.generateMipmap(gl.TEXTURE_2D);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAX_LEVEL, 2);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  groundBuf = gl.createBuffer(); lineBuf = gl.createBuffer();
  // the average colour of the ground picture: far away the ground fades to it instead of shimmering
  const P = window.HYPACK, t = P.tex[P.ground];
  const c = document.createElement('canvas'); c.width = P.aw; c.height = P.ah; const g = c.getContext('2d'); g.drawImage(atlasImage, 0, 0);
  const d = g.getImageData(t[0], t[1], t[2], t[3]).data; let r = 0, gg = 0, b = 0, n = 0;
  for (let i = 0; i < d.length; i += 4) { r += d[i]; gg += d[i + 1]; b += d[i + 2]; n++; }
  groundAvg = [r / n / 255 * 0.92, gg / n / 255 * 0.92, b / n / 255 * 0.92];
  packReady = true;
}

// ============================================================================================
// the sky and time of day
// ============================================================================================
function skyState() {
  const one = [1, 1, 1];
  if (S.sky === 'time') {
    const a = (S.time - 6) / 12 * Math.PI, e = Math.sin(a);
    const sun = norm3([Math.cos(a) * 0.6, Math.sin(a), -Math.cos(a) * 0.8]);
    const day = smooth(-0.14, 0.22, e), dusk = Math.exp(-Math.pow(e / 0.2, 2));
    let top = mix3([0.008, 0.014, 0.05], [0.22, 0.42, 0.78], day);
    let hor = mix3([0.03, 0.05, 0.12], [0.66, 0.79, 0.93], day);
    top = mix3(top, [0.25, 0.27, 0.5], dusk * 0.5);
    hor = mix3(hor, [1.0, 0.52, 0.25], dusk * 0.8);
    let tint = mix3([0.6, 0.7, 1.0], one, day);
    tint = mix3(tint, [1.0, 0.86, 0.72], dusk * 0.6);
    const amb = lerp(0.42, 1.0, day);
    return { top, hor, amb: tint.map(v => v * amb), sun, sunOn: 1, stars: 1 - day, fog: hor, glow: lerp(1.15, 1.0, day), fogOn: 1 };
  }
  if (S.sky === 'studio') return { top: [0.80, 0.83, 0.89], hor: [0.94, 0.95, 0.97], amb: one, sun: [0, 1, 0], sunOn: 0, stars: 0, fog: [0.94, 0.95, 0.97], glow: 1, fogOn: 1 };
  if (S.sky === 'white') return { top: one, hor: one, amb: one, sun: [0, 1, 0], sunOn: 0, stars: 0, fog: one, glow: 1, fogOn: 1 };
  if (S.sky === 'dark') return { top: [0.07, 0.08, 0.11], hor: [0.11, 0.12, 0.16], amb: one, sun: [0, 1, 0], sunOn: 0, stars: 0, fog: [0.11, 0.12, 0.16], glow: 1, fogOn: 1 };
  return { top: [0, 0, 0], hor: [0, 0, 0], amb: one, sun: [0, 1, 0], sunOn: 0, stars: 0, fog: [0, 0, 0], glow: 1, fogOn: 0, clear: true };
}
function timeLabel(h) { const H = Math.floor(h), M = Math.round((h - H) * 60); return String(H % 24).padStart(2, '0') + ':' + String(M === 60 ? 59 : M).padStart(2, '0'); }

// ============================================================================================
// cameras: orbit, free fly, walk
// ============================================================================================
const orbit = { yaw: 0.75, pitch: 0.5, dist: 30, target: [0, 5, 0], vYaw: 0, vPitch: 0 };
const look = { yaw: Math.PI, pitch: 0 };            // fly and walk: forward = (sin yaw cos pitch, sin pitch, cos yaw cos pitch)
const fp = { x: 0, y: 5, z: 0 };                     // fly eye
const player = { x: 0, y: 0, z: 0, vy: 0, onGround: false, crouch: false };
let walkFly = false, eyeH = 1.62;
const spawn = { x: 0, y: 0, z: 0, yaw: Math.PI, pitch: 0.05 };
let anim = null;
const VIEWS = { south: [0, 0.3], north: [Math.PI, 0.3], east: [Math.PI / 2, 0.3], west: [-Math.PI / 2, 0.3], top: [0, 1.5], iso: [0.75, 0.5] };

function sizeOf() { const w = fullMax[0] - fullMin[0] + 1, h = fullMax[1] - fullMin[1] + 1, d = fullMax[2] - fullMin[2] + 1; return { w, h, d, diag: Math.hypot(w, h, d) }; }
function centre() { return [(fullMin[0] + fullMax[0] + 1) / 2, (fullMin[1] + fullMax[1] + 1) / 2, (fullMin[2] + fullMax[2] + 1) / 2]; }
// How far back the orbit camera must be for the whole build to fit the window from a given direction.
function fitOrbit(yaw, pitch) {
  if (!prefab) return { target: orbit.target, dist: orbit.dist };
  yaw = yaw === undefined ? orbit.yaw : yaw; pitch = pitch === undefined ? orbit.pitch : pitch;
  const c = centre(), { w, h } = canvasSize(), asp = w / h, th = Math.tan(S.fov * Math.PI / 360);
  const f = forwardOf(yaw + Math.PI, -pitch), r = [-Math.cos(yaw + Math.PI), 0, Math.sin(yaw + Math.PI)];
  const u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]];
  let need = 6;
  for (let i = 0; i < 8; i++) {
    const p = [(i & 1 ? fullMax[0] + 1 : fullMin[0]) - c[0], (i & 2 ? fullMax[1] + 1 : fullMin[1]) - c[1], (i & 4 ? fullMax[2] + 1 : fullMin[2]) - c[2]];
    const x = p[0] * r[0] + p[2] * r[2], y = p[0] * u[0] + p[1] * u[1] + p[2] * u[2], z = p[0] * f[0] + p[1] * f[1] + p[2] * f[2];   // z: away from the camera
    need = Math.max(need, Math.abs(x) / (th * asp) + z, Math.abs(y) / th + z);
  }
  return { target: c, dist: need * 1.08 + 1 };
}
function animateOrbit(to, secs) {
  anim = { from: { yaw: orbit.yaw, pitch: orbit.pitch, dist: orbit.dist, target: orbit.target.slice() }, to, t: 0, dur: secs || 0.4 };
  want();
}
function setView(v, instant) {
  const [yaw, pitch] = VIEWS[v] || VIEWS.iso;
  const fit = fitOrbit(yaw, pitch);
  // turn the short way round
  let dy = yaw - orbit.yaw; dy = Math.atan2(Math.sin(dy), Math.cos(dy));
  if (S.mode !== 'orbit') setMode('orbit');
  if (instant) { orbit.yaw = yaw; orbit.pitch = pitch; orbit.dist = fit.dist; orbit.target = fit.target; orbit.vYaw = orbit.vPitch = 0; anim = null; want(); }
  else animateOrbit({ yaw: orbit.yaw + dy, pitch, dist: fit.dist, target: fit.target });
}
document.querySelectorAll('button[data-view]').forEach(b => b.onclick = () => setView(b.dataset.view));
$('frame').onclick = frameAll;
function frameAll() { if (S.mode !== 'orbit') setMode('orbit'); const f = fitOrbit(); animateOrbit({ yaw: orbit.yaw, pitch: orbit.pitch, dist: f.dist, target: f.target }); }

function orbitEye() {
  const cp = Math.cos(orbit.pitch);
  return [orbit.target[0] + orbit.dist * Math.sin(orbit.yaw) * cp, orbit.target[1] + orbit.dist * Math.sin(orbit.pitch), orbit.target[2] + orbit.dist * Math.cos(orbit.yaw) * cp];
}
function forwardOf(yaw, pitch) { const cp = Math.cos(pitch); return [Math.sin(yaw) * cp, Math.sin(pitch), Math.cos(yaw) * cp]; }

let viewSize = null;      // forced size for big screenshots
function canvasSize() {
  if (viewSize) return viewSize;
  const dpr = window.devicePixelRatio || 1;
  return { w: Math.max(1, Math.floor(cv.clientWidth * dpr)), h: Math.max(1, Math.floor(cv.clientHeight * dpr)) };
}

function camera() {
  const { w, h } = canvasSize(), asp = w / h, fov = S.fov * Math.PI / 180;
  let eye, f, yawForRight, ortho = false, halfH = 0;
  const big = prefab ? sizeOf().diag : 100;
  const far = Math.max(4000, big * 40);
  if (S.mode === 'orbit') {
    f = forwardOf(orbit.yaw + Math.PI, -orbit.pitch);
    yawForRight = orbit.yaw + Math.PI;
    eye = orbitEye();
    if (S.ortho) {
      ortho = true; halfH = orbit.dist * Math.tan(fov / 2);
      const back = big * 2 + 60;
      eye = [orbit.target[0] - f[0] * back, orbit.target[1] - f[1] * back, orbit.target[2] - f[2] * back];
    }
  } else {
    f = forwardOf(look.yaw, look.pitch); yawForRight = look.yaw;
    eye = S.mode === 'walk' && !walkFly ? [player.x, player.y + eyeH, player.z] : [fp.x, fp.y, fp.z];
  }
  const r = [-Math.cos(yawForRight), 0, Math.sin(yawForRight)];
  const u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]];
  const view = M4.look(eye, [eye[0] + f[0], eye[1] + f[1], eye[2] + f[2]], [0, 1, 0]);
  const proj = ortho ? M4.ortho(halfH * asp, halfH, 0.1, far + big * 4) : M4.persp(fov, asp, 0.06, far);
  return { eye, f, r, u, vp: M4.mul(proj, view), tan: Math.tan(fov / 2), asp, ortho, halfH };
}

// ============================================================================================
// drawing
// ============================================================================================
function wireBox(out, x0, y0, z0, x1, y1, z1) {
  const c = [[x0, y0, z0], [x1, y0, z0], [x1, y0, z1], [x0, y0, z1], [x0, y1, z0], [x1, y1, z0], [x1, y1, z1], [x0, y1, z1]];
  for (const [a, b] of [[0, 1], [1, 2], [2, 3], [3, 0], [4, 5], [5, 6], [6, 7], [7, 4], [0, 4], [1, 5], [2, 6], [3, 7]]) out.push(...c[a], ...c[b]);
}
function drawLines(cam, verts, rgba, xray) {
  if (!verts.length) return;
  const pr = progs.line;
  gl.useProgram(pr.p);
  gl.uniformMatrix4fv(pr.u.uVP, false, cam.vp);
  gl.bindBuffer(gl.ARRAY_BUFFER, lineBuf); gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(verts), gl.DYNAMIC_DRAW);
  const a = pr.a('aPos'); gl.enableVertexAttribArray(a); gl.vertexAttribPointer(a, 3, gl.FLOAT, false, 12, 0);
  gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
  gl.uniform4f(pr.u.uColor, rgba[0], rgba[1], rgba[2], rgba[3]);
  gl.drawArrays(gl.LINES, 0, verts.length / 3);
  if (xray) { gl.disable(gl.DEPTH_TEST); gl.uniform4f(pr.u.uColor, rgba[0], rgba[1], rgba[2], rgba[3] * 0.25); gl.drawArrays(gl.LINES, 0, verts.length / 3); gl.enable(gl.DEPTH_TEST); }
  gl.disable(gl.BLEND);
}

function draw(opts) {
  if (!packReady) return;
  opts = opts || {};
  const { w, h } = canvasSize();
  if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
  gl.viewport(0, 0, w, h);
  const sky = opts.clear ? { ...skyState(), clear: true } : skyState();
  const clearOnly = sky.clear || opts.clear;
  gl.clearColor(0, 0, 0, 0); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
  if (!prefab || !mesh) { drawSkyOnly(sky, clearOnly); return; }
  const cam = camera(), P = window.HYPACK;
  const { diag } = sizeOf();
  const vis = Math.max(250, diag * 4), groundR = vis * 6;
  // fog thickens with the square of the distance: clear around the build, solid at the edge of the ground
  const fogD = clearOnly ? 0 : (0.4 + S.haze / 100 * 1.6) * 1.73 / groundR;
  gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LEQUAL); gl.disable(gl.CULL_FACE);
  gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, atlasTex);

  if (!clearOnly) {
    const sp = progs.sky;
    gl.useProgram(sp.p); gl.disable(gl.DEPTH_TEST); gl.depthMask(false);
    gl.uniform3fv(sp.u.uF, cam.f); gl.uniform3fv(sp.u.uR, cam.r); gl.uniform3fv(sp.u.uU, cam.u);
    gl.uniform1f(sp.u.uTan, cam.tan); gl.uniform1f(sp.u.uAsp, cam.asp); gl.uniform1f(sp.u.uOrtho, cam.ortho ? 1 : 0);
    gl.uniform3fv(sp.u.uTop, sky.top); gl.uniform3fv(sp.u.uHor, sky.hor); gl.uniform3fv(sp.u.uSun, sky.sun);
    gl.uniform1f(sp.u.uSunOn, sky.sunOn); gl.uniform1f(sp.u.uStars, sky.stars);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.depthMask(true); gl.enable(gl.DEPTH_TEST);
  }

  const groundMode = opts.clear ? 'none' : S.ground;
  if (groundMode !== 'none') {
    const gp = progs.ground, R = groundR, cx = Math.round(cam.eye[0]), cz = Math.round(cam.eye[2]);
    gl.useProgram(gp.p);
    const gv = new Float32Array([cx - R, 0, cz - R, cx + R, 0, cz - R, cx + R, 0, cz + R, cx - R, 0, cz - R, cx + R, 0, cz + R, cx - R, 0, cz + R]);
    gl.bindBuffer(gl.ARRAY_BUFFER, groundBuf); gl.bufferData(gl.ARRAY_BUFFER, gv, gl.DYNAMIC_DRAW);
    const loc = gp.a('aPos'); gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc, 3, gl.FLOAT, false, 12, 0);
    const t = P.tex[P.ground];
    gl.uniformMatrix4fv(gp.u.uVP, false, cam.vp);
    gl.uniform4f(gp.u.uRect, t[0], t[1], t[2], t[3]); gl.uniform2f(gp.u.uAtlasSize, P.aw, P.ah);
    gl.uniform3fv(gp.u.uAvg, groundAvg); gl.uniform3fv(gp.u.uAmb, sky.amb); gl.uniform3fv(gp.u.uEye, cam.eye);
    gl.uniform3fv(gp.u.uFog, sky.fog); gl.uniform1f(gp.u.uFogD, fogD);
    gl.uniform1f(gp.u.uMode, groundMode === 'grey' ? 1 : 0); gl.uniform1f(gp.u.uGrid, S.grid ? 1 : 0);
    gl.uniform1i(gp.u.uAtlas, 0);
    gl.drawArrays(gl.TRIANGLES, 0, 6);
  }

  const bp = progs.block, { lo, hi } = effClip();
  gl.useProgram(bp.p);
  gl.uniformMatrix4fv(bp.u.uVP, false, cam.vp);
  gl.uniform1i(bp.u.uAtlas, 0);
  gl.uniform3f(bp.u.uMin, lo[0], lo[1], lo[2]); gl.uniform3f(bp.u.uMax, hi[0], hi[1], hi[2]);
  gl.uniform3fv(bp.u.uAmb, sky.amb); gl.uniform1f(bp.u.uGlow, sky.glow);
  gl.uniform3fv(bp.u.uEye, cam.eye); gl.uniform3fv(bp.u.uFog, sky.fog); gl.uniform1f(bp.u.uFogD, fogD);
  const drawSet = (m, blend) => {
    if (!m) return;
    gl.uniform1f(bp.u.uBlend, blend ? 1 : 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, m.vbo); gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, m.ibo);
    const a = bp.a('aPos'), u = bp.a('aUv'), s = bp.a('aShade');
    gl.enableVertexAttribArray(a); gl.vertexAttribPointer(a, 3, gl.FLOAT, false, 24, 0);
    gl.enableVertexAttribArray(u); gl.vertexAttribPointer(u, 2, gl.FLOAT, false, 24, 12);
    gl.enableVertexAttribArray(s); gl.vertexAttribPointer(s, 1, gl.FLOAT, false, 24, 20);
    gl.drawElements(gl.TRIANGLES, m.count, gl.UNSIGNED_INT, 0);
  };
  drawSet(mesh.opaque, false);
  gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA); gl.depthMask(false);
  drawSet(mesh.blend, true);
  gl.depthMask(true); gl.disable(gl.BLEND);

  if (!opts.clear) {
    if (S.bounds) { const v = []; wireBox(v, fullMin[0], fullMin[1], fullMin[2], fullMax[0] + 1, fullMax[1] + 1, fullMax[2] + 1); drawLines(cam, v, [1, 0.85, 0.3, 0.9], true); }
    if (S.outline && hover && hover !== selected) { const v = []; wireBox(v, hover.x - 0.003, hover.y - 0.003, hover.z - 0.003, hover.x + 1.003, hover.y + 1.003, hover.z + 1.003); drawLines(cam, v, [1, 1, 1, 0.9], true); }
    if (selected) { const v = []; wireBox(v, selected.x - 0.004, selected.y - 0.004, selected.z - 0.004, selected.x + 1.004, selected.y + 1.004, selected.z + 1.004); drawLines(cam, v, [0.4, 0.8, 1, 1], true); }
    if (measure.pts.length) {
      const v = [];
      for (const b of measure.pts) wireBox(v, b.x - 0.004, b.y - 0.004, b.z - 0.004, b.x + 1.004, b.y + 1.004, b.z + 1.004);
      drawLines(cam, v, [1, 0.45, 0.2, 1], true);
      if (measure.pts.length === 2) { const [a, b] = measure.pts; drawLines(cam, [a.x + 0.5, a.y + 0.5, a.z + 0.5, b.x + 0.5, b.y + 0.5, b.z + 0.5], [1, 0.45, 0.2, 1], true); }
    }
  }
  drawCompass(cam);
}
function drawSkyOnly(sky, clearOnly) { /* nothing to draw before a prefab is loaded, except the sky */
  if (clearOnly || !packReady) return;
  const cam = camera(), sp = progs.sky;
  gl.useProgram(sp.p); gl.disable(gl.DEPTH_TEST);
  gl.uniform3fv(sp.u.uF, cam.f); gl.uniform3fv(sp.u.uR, cam.r); gl.uniform3fv(sp.u.uU, cam.u);
  gl.uniform1f(sp.u.uTan, cam.tan); gl.uniform1f(sp.u.uAsp, cam.asp); gl.uniform1f(sp.u.uOrtho, 0);
  gl.uniform3fv(sp.u.uTop, sky.top); gl.uniform3fv(sp.u.uHor, sky.hor); gl.uniform3fv(sp.u.uSun, sky.sun);
  gl.uniform1f(sp.u.uSunOn, sky.sunOn); gl.uniform1f(sp.u.uStars, sky.stars);
  gl.drawArrays(gl.TRIANGLES, 0, 3); gl.enable(gl.DEPTH_TEST);
}

// ---- the compass (north is -z) -----------------------------------------------------------------
const cctx = $('compass').getContext('2d');
function drawCompass(cam) {
  const c = cctx, W = 128, cx = 64, cy = 64, R = 46;
  c.clearRect(0, 0, W, W);
  const heading = Math.atan2(cam.f[0], -cam.f[2]);      // clockwise from north
  c.save(); c.translate(cx, cy);
  c.beginPath(); c.arc(0, 0, R + 10, 0, Math.PI * 2); c.fillStyle = '#0a0c11b0'; c.fill(); c.strokeStyle = '#ffffff30'; c.lineWidth = 2; c.stroke();
  c.font = 'bold 20px system-ui'; c.textAlign = 'center'; c.textBaseline = 'middle';
  for (const [ch, ang, col] of [['N', 0, '#ff6a5a'], ['E', 90, '#e8ecf5'], ['S', 180, '#e8ecf5'], ['W', 270, '#e8ecf5']]) {
    const a = (ang * Math.PI / 180) - heading;
    c.fillStyle = col; c.fillText(ch, Math.sin(a) * R, -Math.cos(a) * R);
  }
  c.beginPath(); c.moveTo(0, -14); c.lineTo(7, 8); c.lineTo(0, 3); c.lineTo(-7, 8); c.closePath(); c.fillStyle = '#ffffffc0'; c.fill();
  c.restore();
}

// ============================================================================================
// picking: walk the ray through the grid of blocks
// ============================================================================================
function inClip(x, y, z) {
  const { lo, hi } = effClip();
  return x >= lo[0] && x <= hi[0] && y >= lo[1] && y <= hi[1] && z >= lo[2] && z <= hi[2];
}
function pickRay(o, d, maxDist) {
  if (!prefab) return null;
  const mn = prefab.min, mx = prefab.max;
  let t0 = 0, t1 = maxDist;
  for (let a = 0; a < 3; a++) {
    if (Math.abs(d[a]) < 1e-9) { if (o[a] < mn[a] || o[a] > mx[a] + 1) return null; continue; }
    let ta = (mn[a] - o[a]) / d[a], tb = (mx[a] + 1 - o[a]) / d[a];
    if (ta > tb) { const s = ta; ta = tb; tb = s; }
    t0 = Math.max(t0, ta); t1 = Math.min(t1, tb);
    if (t0 > t1) return null;
  }
  t0 += 1e-5;
  const p = [o[0] + d[0] * t0, o[1] + d[1] * t0, o[2] + d[2] * t0];
  const cell = [0, 1, 2].map(a => clamp(Math.floor(p[a]), mn[a], mx[a]));
  const step = d.map(v => v > 0 ? 1 : -1), tD = d.map(v => Math.abs(v) < 1e-9 ? Infinity : Math.abs(1 / v));
  const tM = [0, 1, 2].map(a => Math.abs(d[a]) < 1e-9 ? Infinity : ((step[a] > 0 ? cell[a] + 1 : cell[a]) - p[a]) / d[a]);
  let face = [0, 0, 0], t = t0;
  for (let i = 0; i < 4000; i++) {
    const idx = prefab.cellIndex.get(ck(cell[0], cell[1], cell[2]));
    if (idx !== undefined) {
      const b = prefab.blocks[idx];
      if (!hidden.has(b.name) && inClip(b.x, b.y, b.z)) return { block: b, normal: face, t };
    }
    const a = tM[0] < tM[1] ? (tM[0] < tM[2] ? 0 : 2) : (tM[1] < tM[2] ? 1 : 2);
    t = t0 + tM[a]; if (t > t1) return null;
    cell[a] += step[a]; tM[a] += tD[a]; face = [0, 0, 0]; face[a] = -step[a];
    if (cell[a] < mn[a] || cell[a] > mx[a]) return null;
  }
  return null;
}
function rayFromPointer(e) {
  const cam = camera(), rect = cv.getBoundingClientRect();
  const nx = ((e.clientX - rect.left) / rect.width) * 2 - 1, ny = -(((e.clientY - rect.top) / rect.height) * 2 - 1);
  return rayThrough(cam, nx, ny);
}
function rayThrough(cam, nx, ny) {
  if (cam.ortho) {
    const o = cam.eye.map((v, i) => v + cam.r[i] * nx * cam.halfH * cam.asp + cam.u[i] * ny * cam.halfH);
    return { o, d: cam.f };
  }
  const d = norm3(cam.f.map((v, i) => v + cam.r[i] * nx * cam.tan * cam.asp + cam.u[i] * ny * cam.tan));
  return { o: cam.eye, d };
}
function pickAt(nx, ny) {
  const cam = camera(), ray = rayThrough(cam, nx, ny);
  return pickRay(ray.o, ray.d, sizeOf().diag * 40 + 2000);
}

function describe(b) { return prettyName(b.name) + '   at ' + b.x + ', ' + b.y + ', ' + b.z + (b.rot > 0 ? '   rotation ' + b.rot : ''); }
function showTip(text) { const tip = $('tip'); if (text) { tip.style.display = 'block'; tip.textContent = text; } else tip.style.display = 'none'; }

function updateHover(e) {
  if (!prefab || !packReady || S.mode === 'map') return;
  const found = e ? pickPointer(e) : pickAt(0, 0);
  const b = found ? found.block : null;
  if (b !== hover) { hover = b; want(); }
  showTip(b ? describe(b) : '');
}
function pickPointer(e) { const r = rayFromPointer(e); return pickRay(r.o, r.d, sizeOf().diag * 40 + 2000); }

// ---- selecting and measuring ------------------------------------------------------------------
function updateSel() {
  const el = $('sel');
  if (!selected) { el.style.display = 'none'; return; }
  const b = selected, n = counts.get(b.name) || 0;
  el.style.display = 'block';
  el.innerHTML = '<div class="name"></div><div class="dim"></div><div class="row"><button data-a="only">Only this material</button><button data-a="hide">' +
    (hidden.has(b.name) ? 'Show' : 'Hide') + ' it</button><button data-a="go">Look at it</button><button data-a="x">Clear</button></div>';
  el.querySelector('.name').textContent = prettyName(b.name);
  el.querySelector('.dim').textContent = 'at ' + b.x + ', ' + b.y + ', ' + b.z + (b.rot >= 0 ? '   rotation ' + b.rot : '') + '\n' + n.toLocaleString() + ' of this material in the build';
  el.querySelector('.dim').style.whiteSpace = 'pre-line';
  el.onclick = ev => {
    const a = ev.target.dataset && ev.target.dataset.a; if (!a) return;
    if (a === 'only') isolateMaterial(b.name); else if (a === 'hide') toggleMaterial(b.name);
    else if (a === 'go') focusBlock(b); else { selected = null; updateSel(); want(); }
  };
}
function focusBlock(b) {
  if (S.mode === 'map') { map.cx = b.x + 0.5; map.cz = b.z + 0.5; clipMax[1] = b.y; writeSliders(); layerCache.clear(); want(); return; }
  if (S.mode !== 'orbit') setMode('orbit');
  animateOrbit({ yaw: orbit.yaw, pitch: orbit.pitch, dist: Math.max(6, Math.min(orbit.dist, 14)), target: [b.x + 0.5, b.y + 0.5, b.z + 0.5] });
}
function clickBlock(b, shift) {
  if (measure.on) {
    if (!b) return;
    if (measure.pts.length >= 2) measure.pts = [];
    measure.pts.push(b);
    if (measure.pts.length === 2) {
      const [p, q] = measure.pts, dx = q.x - p.x, dy = q.y - p.y, dz = q.z - p.z;
      $('measureOut').textContent = 'east ' + dx + '   up ' + dy + '   south ' + dz + '\nstraight line ' + Math.hypot(dx, dy, dz).toFixed(1) + ' blocks, ' + (Math.abs(dx) + Math.abs(dy) + Math.abs(dz)) + ' by steps';
      $('measureOut').style.whiteSpace = 'pre-line';
    } else $('measureOut').textContent = 'now click the second block';
    want(); return;
  }
  selected = b; updateSel(); want();
}
$('measure').onclick = () => setMeasure(!measure.on);
function setMeasure(on) {
  measure.on = on; $('measure').classList.toggle('on', on);
  if (!on) { measure.pts = []; $('measureOut').textContent = ''; } else $('measureOut').textContent = 'click two blocks';
  want();
}

// ============================================================================================
// modes
// ============================================================================================
function setMode(m) {
  if (m === S.mode) return;
  const prev = S.mode;
  if (document.pointerLockElement) document.exitPointerLock();
  if (prev === 'orbit') {
    // remember where the orbit eye was, so fly starts right there looking the same way
    const eye = orbitEye(), f = forwardOf(orbit.yaw + Math.PI, -orbit.pitch);
    fp.x = eye[0]; fp.y = eye[1]; fp.z = eye[2];
    look.yaw = Math.atan2(f[0], f[2]); look.pitch = Math.asin(clamp(f[1], -1, 1));
  }
  if (prev === 'walk' && !walkFly) { fp.x = player.x; fp.y = player.y + eyeH; fp.z = player.z; }
  S.mode = m; anim = null;
  if (m === 'orbit') {
    // the orbit centre goes where fly was looking
    const d = prefab ? Math.max(6, Math.hypot(fp.x - centre()[0], fp.y - centre()[1], fp.z - centre()[2])) : orbit.dist;
    if (prev === 'fly' || prev === 'walk') {
      const f = forwardOf(look.yaw, look.pitch), dist = clamp(d, 6, 400);
      orbit.target = [fp.x + f[0] * dist, fp.y + f[1] * dist, fp.z + f[2] * dist]; orbit.dist = dist;
      orbit.yaw = look.yaw + Math.PI; orbit.pitch = -look.pitch;
    }
  }
  if (m === 'walk') enterWalk();
  if (m === 'fly' && prev === 'map') { /* keep the last fly position */ }
  applyModeUi(); want();
}
function enterWalk() {
  if (!prefab) return;
  const c = centre(), sz = sizeOf();
  spawn.x = c[0]; spawn.z = fullMax[2] + 4 + Math.min(10, sz.d * 0.1); spawn.y = S.ground === 'none' ? fullMax[1] + 2 : 0;
  spawn.yaw = Math.PI; spawn.pitch = 0.08;
  walkFly = false;
  if (collider) { collider.floor = S.ground === 'none' ? null : 0; collider.clear(); }
  respawn();
}
function respawn() {
  player.x = spawn.x; player.y = spawn.y; player.z = spawn.z; player.vy = 0; player.onGround = false; player.crouch = false;
  look.yaw = spawn.yaw; look.pitch = spawn.pitch; eyeH = HYCOL.EYE; want();
}
function applyModeUi() {
  document.querySelectorAll('#bar button[data-mode]').forEach(b => b.classList.toggle('on', b.dataset.mode === S.mode));
  const isMap = S.mode === 'map', looking = S.mode === 'fly' || S.mode === 'walk';
  mapCv.style.display = isMap ? 'block' : 'none';
  cv.style.display = isMap ? 'none' : 'block';
  cv.className = looking ? 'look' : 'orbit';
  $('compass').style.display = isMap ? 'none' : 'block';
  $('cross').style.display = looking ? 'block' : 'none';
  $('lock').style.display = looking && !document.pointerLockElement && !lockFailed ? 'block' : 'none';
  $('ortho').disabled = S.mode !== 'orbit'; $('spin').disabled = S.mode !== 'orbit';
  updateHint(); showTip('');
  if (isMap) { mapFitIfNeeded(); }
}
document.querySelectorAll('#bar button[data-mode]').forEach(b => b.onclick = () => setMode(b.dataset.mode));
function updateHint() {
  const k = t => '<b>' + t + '</b> ';
  const h = {
    orbit: k('drag') + 'turn  ' + k('right-drag') + 'slide  ' + k('wheel') + 'zoom  ' + k('double-click') + 'centre on a block  ' + k('2') + 'free fly',
    fly: k('WASD') + 'move  ' + k('Space') + 'up  ' + k('Ctrl') + '/' + k('C') + 'down  ' + k('Shift') + 'fast  ' + k('wheel') + 'speed',
    walk: k('WASD') + 'walk  ' + k('Space') + 'jump  ' + k('Ctrl') + '/' + k('C') + 'crouch  ' + k('Shift') + 'sprint  ' + k('V') + 'fly  ' + k('R') + 'restart',
    map: k('drag') + 'slide  ' + k('wheel') + 'zoom  ' + k('[') + k(']') + 'layer  ' + k('Home') + 'fit',
  }[S.mode];
  $('hint').innerHTML = h;
}

// ============================================================================================
// input
// ============================================================================================
const keys = new Set();
let lockFailed = false, pointerDown = null;
const isTyping = e => e.target && /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName) && e.target.type !== 'range' && e.target.type !== 'checkbox';

window.addEventListener('keydown', e => {
  if (isTyping(e)) return;
  const code = e.code;
  if (code === 'Escape') { if ($('help').classList.contains('show')) toggleHelp(false); else { if (selected || measure.on) { selected = null; setMeasure(false); updateSel(); want(); } } return; }
  if (keys.has(code) && e.repeat) { if (['Space', 'ArrowUp', 'ArrowDown'].includes(code)) e.preventDefault(); return; }
  keys.add(code);
  if (['Space', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Tab'].includes(code) && (S.mode !== 'orbit' || code === 'Tab')) e.preventDefault();
  if (e.ctrlKey || e.metaKey || e.altKey) return;
  switch (code) {
    case 'Digit1': setMode('orbit'); break; case 'Digit2': setMode('fly'); break; case 'Digit3': setMode('walk'); break; case 'Digit4': setMode('map'); break;
    case 'Tab': toggleSide(); break;
    case 'KeyH': case 'Slash': toggleHelp(); break;
    case 'BracketLeft': case 'PageDown': moveLayer(-1); e.preventDefault(); break;
    case 'BracketRight': case 'PageUp': moveLayer(1); e.preventDefault(); break;
    case 'KeyM': setMeasure(!measure.on); break;
    case 'KeyP': savePicture(); break;
    case 'KeyG': S.grid = !S.grid; $('grid').checked = S.grid; saveSettings(); want(); break;
    case 'KeyB': S.bounds = !S.bounds; $('bounds').checked = S.bounds; saveSettings(); want(); break;
    case 'KeyF': if (S.mode === 'orbit') frameAll(); break;
    case 'KeyR': if (S.mode === 'orbit') setSpin(!S.spin); else if (S.mode === 'walk') respawn(); break;
    case 'KeyV': if (S.mode === 'walk') { walkFly = !walkFly; if (walkFly) { fp.x = player.x; fp.y = player.y + eyeH; fp.z = player.z; } else { player.x = fp.x; player.y = Math.max(0, fp.y - eyeH); player.z = fp.z; player.vy = 0; } toast(walkFly ? 'flying' : 'walking'); } break;
    case 'Home': if (S.mode === 'map') mapFit(); break;
  }
});
window.addEventListener('keyup', e => keys.delete(e.code));
window.addEventListener('blur', () => keys.clear());

const dragState = { active: false, x: 0, y: 0, pan: false, moved: 0, id: 0, t: 0 };
cv.addEventListener('pointerdown', e => {
  cv.focus();
  dragState.active = true; dragState.x = e.clientX; dragState.y = e.clientY; dragState.moved = 0; dragState.t = performance.now();
  dragState.pan = e.shiftKey || e.button === 2 || e.button === 1; dragState.button = e.button;
  cv.setPointerCapture(e.pointerId);
  if ((S.mode === 'fly' || S.mode === 'walk') && !document.pointerLockElement && e.button === 0) {
    try { const r = cv.requestPointerLock(); if (r && r.catch) r.catch(() => { lockFailed = true; applyModeUi(); }); } catch (err) { lockFailed = true; }
  }
  if (S.mode === 'orbit') { cv.classList.add('dragging'); orbit.vYaw = orbit.vPitch = 0; anim = null; }
});
cv.addEventListener('pointermove', e => {
  if (document.pointerLockElement === cv) return;            // the mouse-look handler below owns it
  if (dragState.active) {
    if (!e.buttons) { dragState.active = false; cv.classList.remove('dragging'); }
    else {
      const dx = e.clientX - dragState.x, dy = e.clientY - dragState.y; dragState.x = e.clientX; dragState.y = e.clientY;
      dragState.moved += Math.abs(dx) + Math.abs(dy);
      if (S.mode === 'orbit') {
        if (dragState.pan) panOrbit(dx, dy);
        else { orbit.yaw -= dx * 0.008; orbit.pitch = clamp(orbit.pitch + dy * 0.008, -1.5, 1.5); orbit.vYaw = -dx * 0.008; orbit.vPitch = dy * 0.008; }
      } else lookBy(dx, dy);
      want(); return;
    }
  }
  updateHover(e);
});
function endDrag(e) {
  if (!dragState.active) return;
  dragState.active = false; cv.classList.remove('dragging');
  const quick = performance.now() - dragState.t < 450;
  if (dragState.moved < 5 && quick && e && e.type === 'pointerup' && dragState.button === 0) {
    const found = document.pointerLockElement ? pickAt(0, 0) : pickPointer(e);
    const now = performance.now();
    if (found && S.mode === 'orbit' && now - (endDrag.last || 0) < 350 && endDrag.lastBlock === found.block) { focusBlock(found.block); endDrag.last = 0; }
    else { clickBlock(found ? found.block : null); endDrag.last = now; endDrag.lastBlock = found ? found.block : null; }
  }
}
cv.addEventListener('pointerup', endDrag);
cv.addEventListener('pointercancel', () => { dragState.active = false; cv.classList.remove('dragging'); });
cv.addEventListener('contextmenu', e => e.preventDefault());
cv.addEventListener('pointerleave', () => { if (!document.pointerLockElement) { hover = null; showTip(''); want(); } });
cv.addEventListener('wheel', e => {
  e.preventDefault();
  if (S.mode === 'orbit') { orbit.dist = clamp(orbit.dist * (e.deltaY < 0 ? 0.9 : 1.11), 1.5, 2500); anim = null; }
  else if (S.mode === 'fly' || (S.mode === 'walk' && walkFly)) { S.flySpeed = clamp(Math.round(S.flySpeed * (e.deltaY < 0 ? 1.15 : 0.87)), 2, 120); $('flyspeed').value = S.flySpeed; $('flyL').textContent = S.flySpeed; saveSettings(); toast('flying speed ' + S.flySpeed + ' blocks/s', 900); }
  want();
}, { passive: false });
document.addEventListener('mousemove', e => { if (document.pointerLockElement === cv) { lookBy(e.movementX, e.movementY); want(); } });
document.addEventListener('pointerlockchange', () => {
  lockFailed = false; applyModeUi(); keys.clear();
  if (document.pointerLockElement !== cv) showTip('');
});
document.addEventListener('pointerlockerror', () => { lockFailed = true; applyModeUi(); toast('The browser would not hide the mouse: drag to look instead', 3000); });
function lookBy(dx, dy) {
  const s = 0.0022 * S.sens / 100 * (S.fov / 70);
  look.yaw -= dx * s;
  look.pitch = clamp(look.pitch - dy * s * (S.invY ? -1 : 1), -1.55, 1.55);
}
function panOrbit(dx, dy) {
  const s = orbit.dist * 0.0016, rx = Math.cos(orbit.yaw), rz = -Math.sin(orbit.yaw);
  orbit.target[0] -= dx * rx * s; orbit.target[2] -= dx * rz * s; orbit.target[1] += dy * s;
}

// the map gets the same drag and wheel
mapCv.addEventListener('pointerdown', e => { map.drag = { x: e.clientX, y: e.clientY, moved: 0 }; mapCv.setPointerCapture(e.pointerId); });
mapCv.addEventListener('pointermove', e => {
  if (map.drag && e.buttons) {
    const dx = e.clientX - map.drag.x, dy = e.clientY - map.drag.y; map.drag.x = e.clientX; map.drag.y = e.clientY; map.drag.moved += Math.abs(dx) + Math.abs(dy);
    map.cx -= dx / map.zoom; map.cz -= dy / map.zoom; want();
  } else { map.drag = null; mapHover(e); }
});
mapCv.addEventListener('pointerup', e => {
  if (map.drag && map.drag.moved < 4) { const b = mapBlockAt(e); if (b) clickBlock(b); }
  map.drag = null;
});
mapCv.addEventListener('wheel', e => {
  e.preventDefault();
  const rect = mapCv.getBoundingClientRect(), mx = e.clientX - rect.left - rect.width / 2, my = e.clientY - rect.top - rect.height / 2;
  const before = [map.cx + mx / map.zoom, map.cz + my / map.zoom];
  map.zoom = clamp(map.zoom * (e.deltaY < 0 ? 1.18 : 0.85), 1.5, 120);
  map.cx = before[0] - mx / map.zoom; map.cz = before[1] - my / map.zoom; want();
}, { passive: false });

// ---- side panel inputs -----------------------------------------------------------------------
function bindRange(id, key, label, fmt) {
  const el = $(id);
  el.value = S[key]; $(label).textContent = fmt ? fmt(S[key]) : S[key];
  el.oninput = () => { S[key] = +el.value; $(label).textContent = fmt ? fmt(S[key]) : S[key]; saveSettings(); want(); };
}
bindRange('fov', 'fov', 'fovL'); bindRange('spinspeed', 'spinSpeed', 'spinL'); bindRange('time', 'time', 'timeL', timeLabel);
bindRange('haze', 'haze', 'hazeL'); bindRange('flyspeed', 'flySpeed', 'flyL'); bindRange('sens', 'sens', 'sensL');
function bindCheck(id, key, after) { const el = $(id); el.checked = S[key]; el.onchange = () => { S[key] = el.checked; saveSettings(); if (after) after(); want(); }; }
bindCheck('ao', 'ao', () => { if (prefab) buildMesh(); }); bindCheck('grid', 'grid'); bindCheck('bounds', 'bounds'); bindCheck('outline', 'outline');
bindCheck('flycollide', 'flyCollide'); bindCheck('invy', 'invY');
function bindSelect(id, key, after) { const el = $(id); el.value = S[key]; el.onchange = () => { S[key] = el.value; saveSettings(); if (after) after(); want(); }; }
bindSelect('sky', 'sky', syncSkyUi); bindSelect('ground', 'ground', () => { if (collider) collider.floor = S.ground === 'none' ? null : 0; });
function syncSkyUi() { $('time').disabled = S.sky !== 'time'; $('haze').disabled = S.sky === 'clear'; }
syncSkyUi();
$('ortho').classList.toggle('on', S.ortho);
$('ortho').onclick = () => { S.ortho = !S.ortho; $('ortho').classList.toggle('on', S.ortho); saveSettings(); want(); };
function setSpin(on) { S.spin = on; $('spin').classList.toggle('on', on); if (on) { if (S.mode !== 'orbit') setMode('orbit'); toast('turntable on (R to stop)'); } want(); }
$('spin').onclick = () => setSpin(!S.spin);
function toggleSide() { S.sideHidden = !S.sideHidden; document.body.classList.toggle('noside', S.sideHidden); saveSettings(); setTimeout(want, 200); }
document.body.classList.toggle('noside', S.sideHidden);
$('sidebtn').onclick = toggleSide;
function toggleHelp(force) { $('help').classList.toggle('show', force === undefined ? !$('help').classList.contains('show') : force); }
$('helpbtn').onclick = () => toggleHelp(true); $('helptop').onclick = () => toggleHelp(); $('helpclose').onclick = () => toggleHelp(false);
$('help').onclick = e => { if (e.target === $('help')) toggleHelp(false); };
window.addEventListener('resize', want);
if (window.ResizeObserver) new ResizeObserver(want).observe($('stage'));

// ============================================================================================
// pictures and video
// ============================================================================================
function savePicture() {
  if (!prefab || !packReady) return;
  const name = prefab.name.replace(/\.json$/i, '');
  if (S.mode === 'map') { drawMap(); mapCv.toBlob(b => download(name + '_layer' + clipMax[1] + '.png', b)); return; }
  const sc = +$('shotscale').value, clear = $('shotclear').checked;
  const { w, h } = canvasSize();
  const W = Math.min(w * sc, 8192), H = Math.min(Math.round(h * W / w), 8192);
  viewSize = { w: W, h: H };
  draw({ clear });
  cv.toBlob(b => { download(name + (sc > 1 ? '@' + sc + 'x' : '') + '.png', b); viewSize = null; want(); toast('Saved ' + W + ' x ' + H + ' picture'); });
}
$('shot').onclick = savePicture;
$('rec').onclick = () => {
  if (!prefab) return;
  if (!window.MediaRecorder || !cv.captureStream) { toast('This browser cannot record the canvas'); return; }
  const wasSpin = S.spin, wasSpeed = S.spinSpeed;
  setMode('orbit'); S.spinSpeed = 36; S.spin = true;
  const rec = new MediaRecorder(cv.captureStream(30), { mimeType: MediaRecorder.isTypeSupported('video/webm;codecs=vp9') ? 'video/webm;codecs=vp9' : 'video/webm', videoBitsPerSecond: 8e6 });
  const chunks = [];
  rec.ondataavailable = e => { if (e.data.size) chunks.push(e.data); };
  rec.onstop = () => { download(prefab.name.replace(/\.json$/i, '') + '_turntable.webm', new Blob(chunks, { type: 'video/webm' })); S.spin = wasSpin; S.spinSpeed = wasSpeed; $('spin').classList.toggle('on', S.spin); toast('Saved the turntable video'); $('rec').disabled = false; };
  $('rec').disabled = true; toast('Recording one full turn (10 s)...', 10000);
  rec.start(); setTimeout(() => rec.stop(), 10000);
};

// ============================================================================================
// layer map: the build one layer at a time, from above
// ============================================================================================
const map = { cx: 0, cz: 0, zoom: 12, drag: null, fitted: false, hover: null };
const layerCache = new Map();           // "layer|clip" -> { canvas, ppc }
let layerLists = null;
function layerBlocks(y) {
  if (!layerLists) { layerLists = new Map(); for (const b of prefab.blocks) { let l = layerLists.get(b.y); if (!l) layerLists.set(b.y, l = []); l.push(b); } }
  return layerLists.get(y) || [];
}
function mapFit() {
  if (!prefab) return;
  const w = fullMax[0] - fullMin[0] + 1, d = fullMax[2] - fullMin[2] + 1, r = mapCv.getBoundingClientRect();
  map.zoom = clamp(Math.min((r.width - 80) / w, (r.height - 80) / d), 1.5, 40);
  map.cx = (fullMin[0] + fullMax[0] + 1) / 2; map.cz = (fullMin[2] + fullMax[2] + 1) / 2; map.fitted = true; want();
}
function mapFitIfNeeded() { if (!map.fitted) requestAnimationFrame(mapFit); want(); }
function renderLayer(y) {
  const { lo, hi } = effClip();
  const key = y + '|' + lo.join(',') + '|' + hi.join(',') + '|' + [...hidden].join(',');
  if (layerCache.has(key)) return layerCache.get(key);
  const w = fullMax[0] - fullMin[0] + 1, d = fullMax[2] - fullMin[2] + 1;
  const ppc = clamp(Math.floor(4096 / Math.max(w, d)), 2, 16);
  const c = document.createElement('canvas'); c.width = w * ppc; c.height = d * ppc;
  const g = c.getContext('2d'); g.imageSmoothingEnabled = false;
  const paint = (yy, alpha) => {
    g.globalAlpha = alpha;
    for (const b of layerBlocks(yy)) {
      if (hidden.has(b.name) || b.x < lo[0] || b.x > hi[0] || b.z < lo[2] || b.z > hi[2]) continue;
      const t = tileOf(b.name);
      g.drawImage(thumbCanvas, t[0], t[1], t[2], t[3], (b.x - fullMin[0]) * ppc, (b.z - fullMin[2]) * ppc, ppc, ppc);
    }
    g.globalAlpha = 1;
  };
  // the layer below shows faintly, so you can see what a block stands on
  if (y - 1 >= fullMin[1]) paint(y - 1, 0.45);
  paint(y, 1);
  const r = { canvas: c, ppc };
  if (layerCache.size > 6) layerCache.delete(layerCache.keys().next().value);
  layerCache.set(key, r);
  return r;
}
function mapBlockAt(e) {
  const rect = mapCv.getBoundingClientRect();
  const x = Math.floor(map.cx + (e.clientX - rect.left - rect.width / 2) / map.zoom), z = Math.floor(map.cz + (e.clientY - rect.top - rect.height / 2) / map.zoom);
  const y = effClip().hi[1];
  const idx = prefab && prefab.cellIndex.get(ck(x, y, z));
  return idx !== undefined ? prefab.blocks[idx] : null;
}
function mapHover(e) {
  if (!prefab) return;
  const rect = mapCv.getBoundingClientRect();
  const x = Math.floor(map.cx + (e.clientX - rect.left - rect.width / 2) / map.zoom), z = Math.floor(map.cz + (e.clientY - rect.top - rect.height / 2) / map.zoom);
  const b = mapBlockAt(e);
  map.hover = { x, z, b };
  showTip(b ? describe(b) : 'x ' + x + '   z ' + z + '   (empty)');
  want();
}
function drawMap() {
  const r = mapCv.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
  const W = Math.floor(r.width * dpr), H = Math.floor(r.height * dpr);
  if (mapCv.width !== W || mapCv.height !== H) { mapCv.width = W; mapCv.height = H; }
  const g = mapCv.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.fillStyle = '#14161c'; g.fillRect(0, 0, r.width, r.height);
  if (!prefab || !thumbCanvas) return;
  const y = effClip().hi[1], z = map.zoom;
  const ox = r.width / 2 - map.cx * z, oy = r.height / 2 - map.cz * z;
  // the footprint of the whole build, so empty cells read as empty rather than missing
  g.fillStyle = '#1b1e27'; g.fillRect(ox + fullMin[0] * z, oy + fullMin[2] * z, (fullMax[0] - fullMin[0] + 1) * z, (fullMax[2] - fullMin[2] + 1) * z);
  const L = renderLayer(y);
  g.imageSmoothingEnabled = false;
  g.drawImage(L.canvas, ox + fullMin[0] * z, oy + fullMin[2] * z, L.canvas.width / L.ppc * z, L.canvas.height / L.ppc * z);
  // grid
  const x0 = Math.max(fullMin[0], Math.floor(-ox / z)), x1 = Math.min(fullMax[0] + 1, Math.ceil((r.width - ox) / z));
  const z0 = Math.max(fullMin[2], Math.floor(-oy / z)), z1 = Math.min(fullMax[2] + 1, Math.ceil((r.height - oy) / z));
  if (z >= 6) {
    g.lineWidth = 1;
    for (let x = x0; x <= x1; x++) { g.strokeStyle = x % 8 === 0 ? '#ffffff38' : '#ffffff14'; g.beginPath(); g.moveTo(ox + x * z + 0.5, oy + z0 * z); g.lineTo(ox + x * z + 0.5, oy + z1 * z); g.stroke(); }
    for (let q = z0; q <= z1; q++) { g.strokeStyle = q % 8 === 0 ? '#ffffff38' : '#ffffff14'; g.beginPath(); g.moveTo(ox + x0 * z, oy + q * z + 0.5); g.lineTo(ox + x1 * z, oy + q * z + 0.5); g.stroke(); }
  } else {
    g.strokeStyle = '#ffffff20'; g.lineWidth = 1;
    for (let x = x0; x <= x1; x++) if (x % 8 === 0) { g.beginPath(); g.moveTo(ox + x * z + 0.5, oy + z0 * z); g.lineTo(ox + x * z + 0.5, oy + z1 * z); g.stroke(); }
    for (let q = z0; q <= z1; q++) if (q % 8 === 0) { g.beginPath(); g.moveTo(ox + x0 * z, oy + q * z + 0.5); g.lineTo(ox + x1 * z, oy + q * z + 0.5); g.stroke(); }
  }
  // which way stairs and roofs climb: the arrow points at the tall side
  if (z >= 14) {
    g.strokeStyle = '#ffd24a'; g.fillStyle = '#ffd24a'; g.lineWidth = 2;
    for (const b of layerBlocks(y)) {
      if (hidden.has(b.name) || !/Stairs|_Roof/.test(b.name) || /Flat/.test(b.name) || b.x < x0 || b.x >= x1 || b.z < z0 || b.z >= z1) continue;
      const rot = b.rot < 0 ? 0 : b.rot; if ((rot & 3) !== rot && rot < 8) continue;
      const dir = [[0, -1], [-1, 0], [0, 1], [1, 0]][rot & 3];
      const cx = ox + (b.x + 0.5) * z, cy = oy + (b.z + 0.5) * z, l = z * 0.3;
      g.beginPath(); g.moveTo(cx - dir[0] * l, cy - dir[1] * l); g.lineTo(cx + dir[0] * l, cy + dir[1] * l); g.stroke();
      g.beginPath(); g.moveTo(cx + dir[0] * l, cy + dir[1] * l); g.lineTo(cx + dir[0] * l * 0.2 - dir[1] * l * 0.6, cy + dir[1] * l * 0.2 + dir[0] * l * 0.6); g.lineTo(cx + dir[0] * l * 0.2 + dir[1] * l * 0.6, cy + dir[1] * l * 0.2 - dir[0] * l * 0.6); g.closePath(); g.fill();
    }
  }
  // outline of the build
  g.strokeStyle = '#ffd24a90'; g.lineWidth = 1.5;
  g.strokeRect(ox + fullMin[0] * z, oy + fullMin[2] * z, (fullMax[0] - fullMin[0] + 1) * z, (fullMax[2] - fullMin[2] + 1) * z);
  // rulers
  g.font = '11px system-ui'; g.fillStyle = '#c9cfdede'; g.textBaseline = 'top';
  const every = z >= 24 ? 1 : z >= 10 ? 2 : z >= 5 ? 5 : z >= 2.5 ? 10 : 20;
  g.textAlign = 'center';
  for (let x = Math.ceil(x0 / every) * every; x < x1; x += every) g.fillText(x, ox + (x + 0.5) * z, Math.max(2, oy + fullMin[2] * z - 14));
  g.textAlign = 'right'; g.textBaseline = 'middle';
  for (let q = Math.ceil(z0 / every) * every; q < z1; q += every) g.fillText(q, Math.max(28, ox + fullMin[0] * z - 4), oy + (q + 0.5) * z);
  // hover and selection
  const box = (b, col) => { g.strokeStyle = col; g.lineWidth = 2; g.strokeRect(ox + b.x * z + 1, oy + b.z * z + 1, z - 2, z - 2); };
  if (map.hover && S.outline) { g.strokeStyle = '#fff'; g.lineWidth = 2; g.strokeRect(ox + map.hover.x * z + 1, oy + map.hover.z * z + 1, z - 2, z - 2); }
  if (selected && selected.y === y) box(selected, '#66ccff');
  for (const b of measure.pts) if (b.y === y) box(b, '#ff7333');
  // the layer label
  g.fillStyle = '#000a'; g.fillRect(12, 50, 150, 28); g.fillStyle = '#fff'; g.font = '13px system-ui'; g.textAlign = 'left'; g.textBaseline = 'middle';
  g.fillText('layer y = ' + y + '   (' + layerBlocks(y).length.toLocaleString() + ' blocks)', 20, 64);
}

// ============================================================================================
// the frame loop
// ============================================================================================
let last = performance.now(), fpsN = 0, fpsT = 0, fps = 0;
function moveInput() {
  const k = c => keys.has(c);
  const f = (k('KeyW') || k('ArrowUp') ? 1 : 0) - (k('KeyS') || k('ArrowDown') ? 1 : 0);
  const s = (k('KeyD') || k('ArrowRight') ? 1 : 0) - (k('KeyA') || k('ArrowLeft') ? 1 : 0);
  const up = (k('Space') ? 1 : 0) - (k('ControlLeft') || k('ControlRight') || k('KeyC') ? 1 : 0);
  return { f, s, up, fast: k('ShiftLeft') || k('ShiftRight') };
}
function tick(dt) {
  dt = Math.min(dt, 0.1);
  let moving = false;
  if (anim) {
    anim.t += dt; const t = smooth(0, 1, anim.t / anim.dur);
    orbit.yaw = lerp(anim.from.yaw, anim.to.yaw, t); orbit.pitch = lerp(anim.from.pitch, anim.to.pitch, t); orbit.dist = lerp(anim.from.dist, anim.to.dist, t);
    orbit.target = anim.from.target.map((v, i) => lerp(v, anim.to.target[i], t));
    if (anim.t >= anim.dur) anim = null;
    moving = true;
  }
  if (S.mode === 'orbit') {
    if (S.spin && !dragState.active) { orbit.yaw += S.spinSpeed * Math.PI / 180 * dt; moving = true; }
    if (!dragState.active && (Math.abs(orbit.vYaw) > 1e-4 || Math.abs(orbit.vPitch) > 1e-4)) {
      orbit.yaw += orbit.vYaw; orbit.pitch = clamp(orbit.pitch + orbit.vPitch, -1.5, 1.5);
      const damp = Math.pow(0.0008, dt); orbit.vYaw *= damp; orbit.vPitch *= damp; moving = true;
    }
    const m = moveInput();
    if (m.f || m.s || m.up) {
      const sp = orbit.dist * 0.6 * dt * (m.fast ? 3 : 1), rx = Math.cos(orbit.yaw), rz = -Math.sin(orbit.yaw), fx = -Math.sin(orbit.yaw), fz = -Math.cos(orbit.yaw);
      orbit.target[0] += (fx * m.f + rx * m.s) * sp; orbit.target[2] += (fz * m.f + rz * m.s) * sp; orbit.target[1] += m.up * sp; moving = true;
    }
  } else if (S.mode === 'fly' || (S.mode === 'walk' && walkFly)) {
    const m = moveInput(), sp = S.flySpeed * (m.fast ? 3 : 1);
    const fx = Math.sin(look.yaw) * Math.cos(look.pitch), fy = Math.sin(look.pitch), fz = Math.cos(look.yaw) * Math.cos(look.pitch), rx = -Math.cos(look.yaw), rz = Math.sin(look.yaw);
    // W flies where you look (up and down included), like the game's camera
    const vx = (fx * m.f + rx * m.s) * sp, vy = (fy * m.f + m.up) * sp, vz = (fz * m.f + rz * m.s) * sp;
    if (vx || vy || vz) {
      if (collider && (S.mode === 'walk' || S.flyCollide)) {
        const p = { x: fp.x, y: fp.y - 1.6, z: fp.z, vy: 0, onGround: false };
        HYCOL.fly(collider, p, dt, { mx: vx, my: vy, mz: vz }); fp.x = p.x; fp.y = p.y + 1.6; fp.z = p.z;
      } else { fp.x += vx * dt; fp.y += vy * dt; fp.z += vz * dt; }
      moving = true;
    }
  } else if (S.mode === 'walk' && collider) {
    const m = moveInput();
    const sp = m.fast ? 7.2 : 4.4, crouch = keys.has('ControlLeft') || keys.has('ControlRight') || keys.has('KeyC');
    const sp2 = crouch ? 1.9 : sp;
    let fx = Math.sin(look.yaw), fz = Math.cos(look.yaw), rx = -Math.cos(look.yaw), rz = Math.sin(look.yaw);
    let mx = fx * m.f + rx * m.s, mz = fz * m.f + rz * m.s; const l = Math.hypot(mx, mz);
    if (l > 1) { mx /= l; mz /= l; }
    HYCOL.step(collider, player, dt, { mx: mx * sp2, mz: mz * sp2, jump: keys.has('Space'), crouch });
    if (player.y < -80) respawn();
    const target = player.crouch ? HYCOL.CROUCH_EYE : HYCOL.EYE; eyeH += (target - eyeH) * Math.min(1, dt * 14);
    moving = true;
  }
  return moving;
}
function updateHud() {
  let t = '';
  if (S.mode === 'orbit') t = 'distance ' + orbit.dist.toFixed(0);
  else if (S.mode === 'fly' || (S.mode === 'walk' && walkFly)) t = 'x ' + fp.x.toFixed(1) + '   y ' + fp.y.toFixed(1) + '   z ' + fp.z.toFixed(1) + '\nflying ' + S.flySpeed + ' blocks/s';
  else if (S.mode === 'walk') t = 'x ' + player.x.toFixed(1) + '   y ' + player.y.toFixed(1) + '   z ' + player.z.toFixed(1) + (player.onGround ? '' : '\nin the air');
  else if (S.mode === 'map') t = 'zoom ' + map.zoom.toFixed(1) + ' px per block';
  $('hud').textContent = t + (t ? '\n' : '') + fps + ' fps';
  $('hud').style.whiteSpace = 'pre-line';
}
function frame(now) {
  requestAnimationFrame(frame);
  const dt = (now - last) / 1000; last = now;
  fpsN++; fpsT += dt; if (fpsT >= 0.5) { fps = Math.round(fpsN / fpsT); fpsN = 0; fpsT = 0; updateHud(); }
  if (!packReady) return;
  const moving = prefab ? tick(dt) : false;
  if (S.mode === 'fly' || S.mode === 'walk') { if (document.pointerLockElement) updateHover(null); }
  if (moving || dirty) {
    dirty = false;
    if (S.mode === 'map') drawMap(); else draw();
  }
}

// ---- files -------------------------------------------------------------------------------------
async function openFile(f) {
  try { loadPrefab(JSON.parse(await f.text()), f.name.replace(/\.json$/i, '')); }
  catch (e) { $('info').textContent = 'could not read that file: ' + e.message; }
}
$('file').addEventListener('change', e => { if (e.target.files[0]) openFile(e.target.files[0]); });
window.addEventListener('dragover', e => { e.preventDefault(); $('drop').classList.add('over'); });
window.addEventListener('dragleave', () => $('drop').classList.remove('over'));
window.addEventListener('drop', e => { e.preventDefault(); $('drop').classList.remove('over'); if (e.dataTransfer.files[0]) openFile(e.dataTransfer.files[0]); });

// ---- start -------------------------------------------------------------------------------------
(async function main() {
  if (window.HYPACK_MISSING || !window.HYPACK) {
    statusEl.innerHTML = 'The block pack (pack.js) is not next to this page.<br>Run <b>python tools/build_preview_pack.py --install</b> once.';
    return;
  }
  try { await initGL(); } catch (e) { statusEl.textContent = 'Could not start the 3D view: ' + e.message; return; }
  statusEl.textContent = 'Drop a prefab .json on this page, or use the button on the left.';
  applyModeUi();
  // for tests and poking around from the console
  window.__dbg = {
    S, orbit, look, fp, player, map, keys,
    setCam(y, p, d) { orbit.yaw = y; orbit.pitch = p; if (d) orbit.dist = d; want(); draw(); },
    setMode, tick, draw: () => (S.mode === 'map' ? drawMap() : draw()), pickAt, get prefab() { return prefab; }, get collider() { return collider; },
    get walkFly() { return walkFly; }, get mesh() { return mesh; }, want, setView,
  };
  if (window.HYPREFAB && !window.HYPREFAB_MISSING) loadPrefab(window.HYPREFAB, (window.HYPREFAB.__name || 'prefab'));
  requestAnimationFrame(frame);
  want();
})();
