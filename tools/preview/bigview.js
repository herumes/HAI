// The page for builds too big for viewer.js: the whole build lives in typed arrays (bigcore.js), the blocks near the camera are
// meshed 32-block chunk by chunk as you fly, everything farther is drawn as coloured points, and a top-down map lets you jump anywhere.
'use strict';
const $ = id => document.getElementById(id);
const cv = $('gl'), mapCv = $('map'), statusEl = $('status');
const S = { fov: 65, time: 16, ground: true, ao: true, radius: 300, speed: 80 };
let gl, atlasTex, atlasImage, core = null, ov = null, packOk = false;
const progs = {};

// ---- maths ------------------------------------------------------------------------------------------
const M4 = {
  mul(a, b) { const o = new Float32Array(16); for (let i = 0; i < 4; i++) for (let j = 0; j < 4; j++) { let s = 0; for (let k = 0; k < 4; k++) s += a[k * 4 + j] * b[i * 4 + k]; o[i * 4 + j] = s; } return o; },
  persp(fov, asp, n, f) { const t = 1 / Math.tan(fov / 2), o = new Float32Array(16); o[0] = t / asp; o[5] = t; o[10] = (f + n) / (n - f); o[11] = -1; o[14] = 2 * f * n / (n - f); return o; },
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
function setStatus(t, sub) { statusEl.style.display = ''; statusEl.innerHTML = t + (sub ? '<small>' + sub + '</small>' : ''); }
const tick = () => new Promise(r => setTimeout(r, 0));

// ---- shaders (the block / ground / sky ones are the same as viewer.js) ----------------------------------
const BLOCK_VS = `#version 300 es
in vec3 aPos; in vec2 aUv; in float aShade;
uniform mat4 uVP; out vec2 vUv; out float vShade; out vec3 vWorld;
void main() { vUv = aUv; vShade = aShade; vWorld = aPos; gl_Position = uVP * vec4(aPos, 1.0); }`;
const BLOCK_FS = `#version 300 es
precision highp float;
in vec2 vUv; in float vShade; in vec3 vWorld;
uniform sampler2D uAtlas; uniform float uBlend; uniform vec3 uAmb; uniform float uGlow; uniform vec3 uEye; uniform vec3 uFog; uniform float uFogD;
out vec4 o;
void main() {
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
const POINT_VS = `#version 300 es
in vec3 aPos; in vec4 aCol; uniform mat4 uVP; uniform float uPx; uniform vec3 uEye; uniform float uCut; out vec4 vC;
void main() {
  vec4 p = uVP * vec4(aPos, 1.0);
  gl_Position = p;
  if (length(aPos - uEye) < uCut) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  gl_PointSize = clamp(uPx * 3.4 / max(p.w, 1.0), 1.6, 8.0);
  vC = aCol;
}`;
const POINT_FS = `#version 300 es
precision highp float; in vec4 vC; uniform vec3 uAmb; out vec4 o;
void main() { o = vec4(vC.rgb * (0.55 + 0.45 * uAmb), 1.0); }`;
const GROUND_VS = `#version 300 es
in vec3 aPos; uniform mat4 uVP; out vec3 vW;
void main() { vW = aPos; gl_Position = uVP * vec4(aPos, 1.0); }`;
const GROUND_FS = `#version 300 es
precision highp float;
in vec3 vW; uniform sampler2D uAtlas; uniform vec4 uRect; uniform vec2 uAtlasSize; uniform vec3 uAvg; uniform vec3 uAmb; uniform vec3 uEye;
out vec4 o;
void main() {
  vec2 uv = vW.xz; float dist = length(vW - uEye);
  vec2 f = fract(uv); vec2 px = uRect.xy + floor(f * uRect.zw) + 0.5;
  vec3 col = texture(uAtlas, px / uAtlasSize).rgb * 0.92;
  col = mix(col, uAvg, smoothstep(24.0, 100.0, dist)) * uAmb;
  o = vec4(col, 1.0);
}`;
const SKY_VS = `#version 300 es
out vec2 vP;
void main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); vP = p * 2.0 - 1.0; gl_Position = vec4(vP, 1.0, 1.0); }`;
const SKY_FS = `#version 300 es
precision highp float;
in vec2 vP; uniform vec3 uF; uniform vec3 uR; uniform vec3 uU; uniform float uTan; uniform float uAsp;
uniform vec3 uTop; uniform vec3 uHor; uniform vec3 uSun; uniform float uStars;
out vec4 o;
void main() {
  vec3 d = normalize(uF + uR * vP.x * uTan * uAsp + uU * vP.y * uTan);
  float h = d.y;
  vec3 col = mix(uHor, uTop, pow(clamp(h, 0.0, 1.0), 0.5));
  col = mix(col, uHor * 0.9, clamp(-h * 5.0, 0.0, 1.0));
  float s = max(dot(d, uSun), 0.0);
  col += vec3(1.0, 0.93, 0.75) * (pow(s, 1400.0) * 3.0 + pow(s, 14.0) * 0.16);
  if (uStars > 0.01 && h > 0.0) {
    vec3 q = floor(d * 420.0);
    float n = fract(sin(dot(q, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
    col += vec3(step(0.9975, n)) * uStars * smoothstep(0.0, 0.15, h);
  }
  o = vec4(col, 1.0);
}`;
function compile(vs, fs) {
  const mk = (t, s) => { const sh = gl.createShader(t); gl.shaderSource(sh, s); gl.compileShader(sh); if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(sh)); return sh; };
  const p = gl.createProgram(); gl.attachShader(p, mk(gl.VERTEX_SHADER, vs)); gl.attachShader(p, mk(gl.FRAGMENT_SHADER, fs)); gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
  const u = {}; const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < n; i++) { const info = gl.getActiveUniform(p, i); u[info.name] = gl.getUniformLocation(p, info.name); }
  return { p, u, a: n => gl.getAttribLocation(p, n) };
}
function skyState() {
  const one = [1, 1, 1], a = (S.time - 6) / 12 * Math.PI, e = Math.sin(a);
  const sun = norm3([Math.cos(a) * 0.6, Math.sin(a), -Math.cos(a) * 0.8]);
  const day = smooth(-0.14, 0.22, e), dusk = Math.exp(-Math.pow(e / 0.2, 2));
  let top = mix3([0.008, 0.014, 0.05], [0.22, 0.42, 0.78], day);
  let hor = mix3([0.03, 0.05, 0.12], [0.66, 0.79, 0.93], day);
  top = mix3(top, [0.25, 0.27, 0.5], dusk * 0.5);
  hor = mix3(hor, [1.0, 0.52, 0.25], dusk * 0.8);
  let tint = mix3([0.6, 0.7, 1.0], one, day);
  tint = mix3(tint, [1.0, 0.86, 0.72], dusk * 0.6);
  const amb = lerp(0.45, 1.0, day);
  return { top, hor, amb: tint.map(v => v * amb), sun, stars: 1 - day, fog: hor, glow: lerp(1.15, 1.0, day) };
}

// ---- the camera ---------------------------------------------------------------------------------------
const cam = { x: 0, y: 200, z: 0, yaw: Math.PI, pitch: -0.6 };
const keys = new Set();
const forwardOf = (yaw, pitch) => { const cp = Math.cos(pitch); return [Math.sin(yaw) * cp, Math.sin(pitch), Math.cos(yaw) * cp]; };
function overviewCam() {
  const ext = Math.max(core.max[0] - core.min[0], core.max[2] - core.min[2]);
  cam.x = (core.min[0] + core.max[0]) / 2; cam.z = (core.min[2] + core.max[2]) / 2 + ext * 0.12; cam.y = ext * 0.95;
  cam.yaw = Math.PI; cam.pitch = -1.25;
}
function frustum(vp) {
  const m = vp, pl = [];
  const add = (a, b, c, d) => { const l = Math.hypot(a, b, c) || 1; pl.push([a / l, b / l, c / l, d / l]); };
  add(m[3] + m[0], m[7] + m[4], m[11] + m[8], m[15] + m[12]); add(m[3] - m[0], m[7] - m[4], m[11] - m[8], m[15] - m[12]);
  add(m[3] + m[1], m[7] + m[5], m[11] + m[9], m[15] + m[13]); add(m[3] - m[1], m[7] - m[5], m[11] - m[9], m[15] - m[13]);
  add(m[3] + m[2], m[7] + m[6], m[11] + m[10], m[15] + m[14]); add(m[3] - m[2], m[7] - m[6], m[11] - m[10], m[15] - m[14]);
  return pl;
}
const boxVisible = (pl, x0, y0, z0, x1, y1, z1) => {
  for (const p of pl) {
    const x = p[0] >= 0 ? x1 : x0, y = p[1] >= 0 ? y1 : y0, z = p[2] >= 0 ? z1 : z0;
    if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) return false;
  }
  return true;
};

// ---- chunk streaming ------------------------------------------------------------------------------------
let loaded = 0, wantList = [], lastScan = -1e9, lastScanPos = [1e9, 0, 1e9], buildMs = 0;
function freeChunk(c) {
  if (c.gl) { for (const s of c.gl) { gl.deleteVertexArray(s.vao); gl.deleteBuffer(s.vbo); gl.deleteBuffer(s.ibo); } loaded--; }
  c.gl = null; c.state = 0;
}
function upload(set) {
  if (!set.i.length) return null;
  const vao = gl.createVertexArray(), vbo = gl.createBuffer(), ibo = gl.createBuffer();
  gl.bindVertexArray(vao);
  gl.bindBuffer(gl.ARRAY_BUFFER, vbo); gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(set.v), gl.STATIC_DRAW);
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ibo); gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(set.i), gl.STATIC_DRAW);
  const bp = progs.block, a = bp.a('aPos'), u = bp.a('aUv'), s = bp.a('aShade');
  gl.enableVertexAttribArray(a); gl.vertexAttribPointer(a, 3, gl.FLOAT, false, 24, 0);
  gl.enableVertexAttribArray(u); gl.vertexAttribPointer(u, 2, gl.FLOAT, false, 24, 12);
  gl.enableVertexAttribArray(s); gl.vertexAttribPointer(s, 1, gl.FLOAT, false, 24, 20);
  gl.bindVertexArray(null);
  return { vao, vbo, ibo, count: set.i.length };
}
function scan(now) {
  lastScan = now; lastScanPos = [cam.x, cam.y, cam.z];
  const R = S.radius, CH = HYBIGCORE.CH;
  wantList = [];
  for (const c of core.chunks) {
    const dx = Math.max(c.x0 - cam.x, 0, cam.x - (c.x0 + CH)), dy = Math.max(c.y0 - cam.y, 0, cam.y - (c.y0 + CH)), dz = Math.max(c.z0 - cam.z, 0, cam.z - (c.z0 + CH));
    const d = Math.hypot(dx, dy, dz);
    if (d <= R) { if (c.state === 0) wantList.push([d, c]); }
    else if (c.gl && d > R * 1.4) freeChunk(c);
  }
  wantList.sort((a, b) => a[0] - b[0]);
}
function pump(budgetMs) {
  const t0 = performance.now();
  while (wantList.length && performance.now() - t0 < budgetMs && loaded < 2600) {
    const c = wantList.shift()[1];
    if (c.state !== 0) continue;
    const m = HYBIGCORE.buildChunk(core, c, null, { ao: S.ao });
    const o = upload(m.opaque), b = upload(m.blend);
    const parts = [];
    if (o) { o.blend = false; parts.push(o); }
    if (b) { b.blend = true; parts.push(b); }
    c.gl = parts;
    c.state = 1; loaded++;
  }
  buildMs = performance.now() - t0;
}
function reloadAll() { for (const c of core.chunks) freeChunk(c); lastScan = -1e9; }

// ---- drawing ------------------------------------------------------------------------------------------------
let groundBuf, ptsVao, ptsN = 0, groundAvg = [0.4, 0.6, 0.25], fps = 0, frames = 0, fpsT = performance.now();
function draw(now) {
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.floor(cv.clientWidth * dpr)), h = Math.max(1, Math.floor(cv.clientHeight * dpr));
  if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
  gl.viewport(0, 0, w, h);
  gl.clearColor(0, 0, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
  const sky = skyState(), asp = w / h, fov = S.fov * Math.PI / 180;
  const f = forwardOf(cam.yaw, cam.pitch), r = [-Math.cos(cam.yaw), 0, Math.sin(cam.yaw)];
  const u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]];
  const eye = [cam.x, cam.y, cam.z];
  const far = 30000;
  const vp = M4.mul(M4.persp(fov, asp, 0.3, far), M4.look(eye, [eye[0] + f[0], eye[1] + f[1], eye[2] + f[2]], [0, 1, 0]));
  gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LEQUAL); gl.disable(gl.CULL_FACE);
  gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, atlasTex);

  const sp = progs.sky;
  gl.useProgram(sp.p); gl.disable(gl.DEPTH_TEST); gl.depthMask(false);
  gl.uniform3fv(sp.u.uF, f); gl.uniform3fv(sp.u.uR, r); gl.uniform3fv(sp.u.uU, u);
  gl.uniform1f(sp.u.uTan, Math.tan(fov / 2)); gl.uniform1f(sp.u.uAsp, asp);
  gl.uniform3fv(sp.u.uTop, sky.top); gl.uniform3fv(sp.u.uHor, sky.hor); gl.uniform3fv(sp.u.uSun, sky.sun); gl.uniform1f(sp.u.uStars, sky.stars);
  gl.drawArrays(gl.TRIANGLES, 0, 3);
  gl.depthMask(true); gl.enable(gl.DEPTH_TEST);

  if (S.ground) {
    const gp = progs.ground, R = 40000, P = window.HYPACK, t = P.tex[P.ground], cx = Math.round(cam.x), cz = Math.round(cam.z);
    gl.useProgram(gp.p);
    gl.bindBuffer(gl.ARRAY_BUFFER, groundBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([cx - R, -0.05, cz - R, cx + R, -0.05, cz - R, cx + R, -0.05, cz + R, cx - R, -0.05, cz - R, cx + R, -0.05, cz + R, cx - R, -0.05, cz + R]), gl.DYNAMIC_DRAW);
    const loc = gp.a('aPos'); gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc, 3, gl.FLOAT, false, 12, 0);
    gl.uniformMatrix4fv(gp.u.uVP, false, vp); gl.uniform4f(gp.u.uRect, t[0], t[1], t[2], t[3]); gl.uniform2f(gp.u.uAtlasSize, P.aw, P.ah);
    gl.uniform3fv(gp.u.uAvg, groundAvg); gl.uniform3fv(gp.u.uAmb, sky.amb); gl.uniform3fv(gp.u.uEye, eye); gl.uniform1i(gp.u.uAtlas, 0);
    gl.drawArrays(gl.TRIANGLES, 0, 6);
  }

  // far away: one coloured point per 4-block bucket (the ones inside the detail radius are clipped away in the shader)
  const pp = progs.points;
  gl.useProgram(pp.p); gl.bindVertexArray(ptsVao);
  gl.uniformMatrix4fv(pp.u.uVP, false, vp); gl.uniform1f(pp.u.uPx, h / (2 * Math.tan(fov / 2)));
  gl.uniform3fv(pp.u.uEye, eye); gl.uniform1f(pp.u.uCut, S.radius * 0.92); gl.uniform3fv(pp.u.uAmb, sky.amb);
  gl.drawArrays(gl.POINTS, 0, ptsN);
  gl.bindVertexArray(null);

  // near: the chunk meshes
  const bp = progs.block, pl = frustum(vp), CH = HYBIGCORE.CH;
  gl.useProgram(bp.p);
  gl.uniformMatrix4fv(bp.u.uVP, false, vp); gl.uniform1i(bp.u.uAtlas, 0);
  gl.uniform3fv(bp.u.uAmb, sky.amb); gl.uniform1f(bp.u.uGlow, sky.glow);
  gl.uniform3fv(bp.u.uEye, eye); gl.uniform3fv(bp.u.uFog, sky.fog); gl.uniform1f(bp.u.uFogD, 1.0 / (S.radius * 1.5));
  const vis = [];
  for (const c of core.chunks) if (c.gl && boxVisible(pl, c.x0, c.y0, c.z0, c.x0 + CH, c.y0 + CH, c.z0 + CH)) vis.push(c);
  gl.uniform1f(bp.u.uBlend, 0);
  for (const c of vis) for (const s of c.gl) if (!s.blend) { gl.bindVertexArray(s.vao); gl.drawElements(gl.TRIANGLES, s.count, gl.UNSIGNED_INT, 0); }
  gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA); gl.depthMask(false); gl.uniform1f(bp.u.uBlend, 1);
  for (const c of vis) for (const s of c.gl) if (s.blend) { gl.bindVertexArray(s.vao); gl.drawElements(gl.TRIANGLES, s.count, gl.UNSIGNED_INT, 0); }
  gl.depthMask(true); gl.disable(gl.BLEND); gl.bindVertexArray(null);
  return vis.length;
}

// ---- the map ------------------------------------------------------------------------------------------------
let mapBase = null, mapScale = 1;
function buildMap() {
  const maxW = 330, sc = Math.min(1, maxW / ov.w);
  const base = document.createElement('canvas'); base.width = ov.w; base.height = ov.h;
  const g = base.getContext('2d'), img = g.createImageData(ov.w, ov.h);
  for (let i = 0; i < ov.w * ov.h; i++) {
    const y = ov.topY[i];
    if (y === -32768) { img.data[i * 4] = 8; img.data[i * 4 + 1] = 10; img.data[i * 4 + 2] = 16; img.data[i * 4 + 3] = 255; continue; }
    const c = ov.col[i], k = 0.55 + 0.45 * clamp((y + 5) / 200, 0, 1);
    img.data[i * 4] = Math.min(255, (c & 255) * k); img.data[i * 4 + 1] = Math.min(255, ((c >> 8) & 255) * k);
    img.data[i * 4 + 2] = Math.min(255, ((c >> 16) & 255) * k); img.data[i * 4 + 3] = 255;
  }
  g.putImageData(img, 0, 0);
  mapBase = base; mapScale = sc;
  mapCv.width = Math.round(ov.w * sc); mapCv.height = Math.round(ov.h * sc);
}
function drawMap() {
  if (!mapBase) return;
  const g = mapCv.getContext('2d');
  g.imageSmoothingEnabled = true; g.drawImage(mapBase, 0, 0, mapCv.width, mapCv.height);
  const px = (cam.x - core.min[0]) / ov.cell * mapScale, pz = (cam.z - core.min[2]) / ov.cell * mapScale;
  const f = forwardOf(cam.yaw, 0);
  g.strokeStyle = '#ffffff'; g.fillStyle = '#ff3b6b'; g.lineWidth = 1.5;
  g.beginPath(); g.moveTo(px, pz); g.lineTo(px + f[0] * 14, pz + f[2] * 14); g.stroke();
  g.beginPath(); g.arc(px, pz, 3.5, 0, 6.3); g.fill(); g.stroke();
}
mapCv.addEventListener('pointerdown', e => {
  if (!core) return;
  const rc = mapCv.getBoundingClientRect(), mx = (e.clientX - rc.left) / rc.width * ov.w, mz = (e.clientY - rc.top) / rc.height * ov.h;
  const wx = core.min[0] + mx * ov.cell, wz = core.min[2] + mz * ov.cell;
  let top = -32768;
  for (let dz = -3; dz <= 3; dz++) for (let dx = -3; dx <= 3; dx++) {
    const ix = Math.floor(mx) + dx, iz = Math.floor(mz) + dz;
    if (ix >= 0 && iz >= 0 && ix < ov.w && iz < ov.h) top = Math.max(top, ov.topY[ix + iz * ov.w]);
  }
  cam.x = wx; cam.z = wz; cam.y = Math.max(top, 0) + 45; cam.pitch = -0.35;
  lastScan = -1e9;
});

// ---- input ----------------------------------------------------------------------------------------------------
let dragging = false;
cv.addEventListener('pointerdown', e => { dragging = true; cv.classList.add('drag'); cv.setPointerCapture(e.pointerId); });
cv.addEventListener('pointerup', () => { dragging = false; cv.classList.remove('drag'); });
cv.addEventListener('pointermove', e => {
  if (!dragging) return;
  cam.yaw -= e.movementX * 0.0035; cam.pitch = clamp(cam.pitch - e.movementY * 0.0035, -1.55, 1.55);
});
cv.addEventListener('wheel', e => { e.preventDefault(); setSpeed(S.speed * (e.deltaY < 0 ? 1.25 : 0.8)); }, { passive: false });
function setSpeed(v) { S.speed = clamp(Math.round(v), 10, 1200); $('spd').value = S.speed; $('spdv').textContent = S.speed; }
window.addEventListener('keydown', e => {
  if (e.target.tagName === 'INPUT') return;
  const k = e.key.toLowerCase();
  keys.add(k);
  if (e.ctrlKey) keys.add('control');
  if (k === 'o' || k === 'r') overviewCam();
  if (k === 't') cycleTime();
  if (k === 'g') S.ground = !S.ground;
  if (k === 'q') { S.ao = !S.ao; reloadAll(); }
  if (k === ' ' || k.startsWith('arrow')) e.preventDefault();
});
window.addEventListener('keyup', e => { keys.delete(e.key.toLowerCase()); if (!e.ctrlKey) keys.delete('control'); });
window.addEventListener('blur', () => keys.clear());
function cycleTime() { const order = [16, 20, 2, 9]; const i = order.findIndex(v => v === S.time); S.time = order[(i + 1) % order.length]; }
$('rad').oninput = () => { S.radius = +$('rad').value; $('radv').textContent = S.radius; lastScan = -1e9; };
$('spd').oninput = () => setSpeed(+$('spd').value);
$('bOver').onclick = overviewCam; $('bTime').onclick = cycleTime; $('bGround').onclick = () => { S.ground = !S.ground; };
$('bAo').onclick = () => { S.ao = !S.ao; reloadAll(); };

// ---- start ----------------------------------------------------------------------------------------------------
let lastT = performance.now();
function frame(now) {
  const dt = Math.min(0.1, (now - lastT) / 1000); lastT = now;
  const f = forwardOf(cam.yaw, cam.pitch), r = [-Math.cos(cam.yaw), 0, Math.sin(cam.yaw)];
  const sp = S.speed * (keys.has('shift') ? 4 : 1) * dt;
  let mx = 0, my = 0, mz = 0;
  if (keys.has('w')) { mx += f[0]; my += f[1]; mz += f[2]; }
  if (keys.has('s')) { mx -= f[0]; my -= f[1]; mz -= f[2]; }
  if (keys.has('d')) { mx += r[0]; mz += r[2]; }
  if (keys.has('a')) { mx -= r[0]; mz -= r[2]; }
  if (keys.has(' ')) my += 1;
  if (keys.has('c') || keys.has('control')) my -= 1;
  cam.x += mx * sp; cam.y += my * sp; cam.z += mz * sp;
  if (now - lastScan > 250 && Math.hypot(cam.x - lastScanPos[0], cam.y - lastScanPos[1], cam.z - lastScanPos[2]) > 6 || lastScan < 0 || (!wantList.length && now - lastScan > 1500)) scan(now);
  pump(10);
  const vis = draw(now);
  drawMap();
  frames++;
  if (now - fpsT > 500) { fps = Math.round(frames * 1000 / (now - fpsT)); frames = 0; fpsT = now; }
  $('hud').innerHTML = '<b>' + core.data.name + '</b>\n' + core.data.n.toLocaleString() + ' blocks, ' + (core.max[0] - core.min[0] + 1) + ' x ' + (core.max[1] - core.min[1] + 1) +
    ' x ' + (core.max[2] - core.min[2] + 1) + '\nyou: x ' + Math.round(cam.x) + '  y ' + Math.round(cam.y) + '  z ' + Math.round(cam.z) +
    '\ndetail chunks: ' + loaded + ' loaded, ' + vis + ' in view, ' + wantList.length + ' waiting   ' + fps + ' fps';
  requestAnimationFrame(frame);
}

async function start() {
  if (window.HYPACK_MISSING || !window.HYPACK) { setStatus('The block pack is missing.', 'Run  python tools\\build_preview_pack.py --install  once.'); return; }
  if (window.HYBIG_MISSING || !window.HYBIG) { setStatus('No big build to show.', 'Use the Preview in 3D button in the mod.'); return; }
  try {
    gl = cv.getContext('webgl2', { antialias: true });
    if (!gl) throw new Error('This browser has no WebGL2.');
    progs.block = compile(BLOCK_VS, BLOCK_FS); progs.ground = compile(GROUND_VS, GROUND_FS); progs.sky = compile(SKY_VS, SKY_FS); progs.points = compile(POINT_VS, POINT_FS);
    atlasImage = new Image(); atlasImage.src = window.HYPACK.atlas; await atlasImage.decode();
    atlasTex = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, atlasTex);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, atlasImage);
    gl.generateMipmap(gl.TEXTURE_2D);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAX_LEVEL, 2);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    groundBuf = gl.createBuffer();
    const P = window.HYPACK, c0 = document.createElement('canvas'); c0.width = P.aw; c0.height = P.ah;
    const g0 = c0.getContext('2d', { willReadFrequently: true }); g0.drawImage(atlasImage, 0, 0);
    // the ground's average colour, and one average colour per block name for the far points and the map
    const avg = (t) => {
      const d = g0.getImageData(t[0], t[1], t[2], t[3]).data; let r = 0, gg = 0, b = 0, n = 0;
      for (let i = 0; i < d.length; i += 4) if (d[i + 3] > 20) { r += d[i]; gg += d[i + 1]; b += d[i + 2]; n++; }
      return n ? [r / n, gg / n, b / n] : [160, 160, 160];
    };
    const ga = avg(P.tex[P.ground]); groundAvg = [ga[0] / 255 * 0.92, ga[1] / 255 * 0.92, ga[2] / 255 * 0.92];
    setStatus('Reading the build...', 'decoding ' + window.HYBIG.n.toLocaleString() + ' blocks'); await tick();
    const data = HYBIGCORE.decode(window.HYBIG); window.HYBIG.data = null;
    setStatus('Indexing the build...', 'a grid of every block and the 32-block chunks'); await tick();
    core = HYBIGCORE.prepare(data, P);
    setStatus('Colouring the far view...', core.chunks.length.toLocaleString() + ' chunks'); await tick();
    const rgb = new Uint8Array(data.names.length * 3);
    data.names.forEach((nm, i) => {
      const rec = core.recs[i], v = rec.v[0];
      let tid = v.c ? (v.c[0] >= 0 ? v.c[0] : v.c[2]) : v.t;
      if (tid === undefined || tid < 0) tid = P.missing;
      const a = avg(P.tex[tid]), boost = (rec.u || rec.l) ? 1.35 : 1;
      rgb[i * 3] = Math.min(255, a[0] * boost); rgb[i * 3 + 1] = Math.min(255, a[1] * boost); rgb[i * 3 + 2] = Math.min(255, a[2] * boost);
    });
    ov = HYBIGCORE.overview(core, rgb, 4);
    ptsVao = gl.createVertexArray(); gl.bindVertexArray(ptsVao);
    const pb = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, pb); gl.bufferData(gl.ARRAY_BUFFER, ov.pos, gl.STATIC_DRAW);
    const a1 = progs.points.a('aPos'); gl.enableVertexAttribArray(a1); gl.vertexAttribPointer(a1, 3, gl.FLOAT, false, 12, 0);
    const cb = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, cb); gl.bufferData(gl.ARRAY_BUFFER, ov.pcol, gl.STATIC_DRAW);
    const a2 = progs.points.a('aCol'); gl.enableVertexAttribArray(a2); gl.vertexAttribPointer(a2, 4, gl.UNSIGNED_BYTE, true, 4, 0);
    gl.bindVertexArray(null); ptsN = ov.np;
    buildMap(); overviewCam();
    statusEl.style.display = 'none';
    if (core.unknown.length) console.log('blocks the pack does not know:', core.unknown);
    requestAnimationFrame(frame);
  } catch (err) {
    setStatus('Could not show this build.', String(err && err.message || err));
    console.error(err);
  }
}
start();
