// GLSL shared by the terrain, sky, cloud and particle materials.

import { DETAIL } from './terrainData.js';

const f = (v) => (Number.isInteger(v) ? v.toFixed(1) : String(v));

export const ENV_UNIFORMS = /* glsl */ `
uniform float uTime;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform float uSunElevDeg;
uniform int uSunBinA;
uniform int uSunBinB;
uniform float uSunBinT;
uniform vec3 uMoonDir;
uniform vec3 uMoonColor;
uniform vec3 uSkyZenith;
uniform vec3 uSkyHorizon;
uniform vec3 uSunsetColor;
uniform float uTwilight;
uniform float uNight;
uniform vec3 uAmbientSky;
uniform vec3 uAmbientGround;
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform float uCloudFog;
uniform float uCloudCover;
uniform float uCloudBase;
uniform float uCloudTop;
uniform float uStorm;
uniform vec2 uWind;
uniform float uWindSpeed;
uniform float uFreshSnow;
uniform float uSnowfall;
uniform vec3 uLampPos;
uniform vec3 uLampDir;
uniform float uLampOn;
uniform vec3 uPlayerPos;
`;

// Distance fog with aerial perspective. Air thins with height (scale height ~8 km for density, less
// for haze), so looking up a mountain is clearer than looking along a valley.
export const FOG = /* glsl */ `
vec3 applyFog(vec3 color, vec3 worldPos, vec3 eye) {
  vec3 ray = worldPos - eye;
  float dist = length(ray);
  float meanHeight = 0.5 * (worldPos.y + eye.y) - eye.y;
  float density = uFogDensity * exp(-meanHeight / 4200.0) + uCloudFog / 70.0;
  float amount = 1.0 - exp(-dist * density);
  float towardSun = max(dot(ray / max(dist, 1.0), uSunDir), 0.0);
  vec3 fogColor = uFogColor + uSunColor * (0.10 * pow(towardSun, 6.0)) * (1.0 - uStorm);
  return mix(color, fogColor, clamp(amount, 0.0, 1.0));
}
`;

export const NOISE = /* glsl */ `
float hash2(ivec2 p) {
  uint h = (uint(p.x) * 0x9E3779B1u) ^ (uint(p.y) * 0x85EBCA77u);
  h ^= h >> 15u;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12u;
  h *= 0x297A2D39u;
  h ^= h >> 15u;
  return float(h) * (1.0 / 4294967296.0);
}
float valueNoise(vec2 p) {
  vec2 i = floor(p);
  vec2 fr = p - i;
  vec2 u = fr * fr * (3.0 - 2.0 * fr);
  ivec2 ip = ivec2(i);
  float a = hash2(ip);
  float b = hash2(ip + ivec2(1, 0));
  float c = hash2(ip + ivec2(0, 1));
  float d = hash2(ip + ivec2(1, 1));
  return a + (b - a) * u.x + (c - a) * u.y + (a - b - c + d) * u.x * u.y;
}
`;

const o = DETAIL.octaves;
const r = DETAIL.roughness;

// Terrain height on the GPU. Must match TerrainData.height() on the CPU.
export const TERRAIN_HEIGHT = /* glsl */ `
uniform sampler2D uHeight;
uniform sampler2D uFar;
uniform sampler2D uMasks;
uniform float uNearSize;
uniform float uNearExtent;
uniform float uFarSize;
uniform float uFarExtent;
uniform float uCoarseSize;
uniform float uGridQuads;

float bilinearHeight(sampler2D tex, float size, float extent, vec2 xz) {
  vec2 p = clamp((xz / extent + 0.5) * (size - 1.0), vec2(0.0), vec2(size - 1.001));
  ivec2 i = ivec2(p);
  vec2 w = p - vec2(i);
  float a = texelFetch(tex, i, 0).r;
  float b = texelFetch(tex, i + ivec2(1, 0), 0).r;
  float c = texelFetch(tex, i + ivec2(0, 1), 0).r;
  float d = texelFetch(tex, i + ivec2(1, 1), 0).r;
  return mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
}

// 1 inside the detailed map, fading to 0 where the low-resolution backdrop takes over.
float nearWeight(vec2 xz) {
  vec2 q = abs(xz) / (uNearExtent * 0.5);
  return 1.0 - smoothstep(0.955, 0.995, max(q.x, q.y));
}

float baseHeight(vec2 xz, float wNear) {
  float h = 0.0;
  if (wNear > 0.0) h += wNear * bilinearHeight(uHeight, uNearSize, uNearExtent, xz);
  if (wNear < 1.0) h += (1.0 - wNear) * bilinearHeight(uFar, uFarSize, uFarExtent, xz);
  return h;
}

vec2 coarseUv(vec2 xz) {
  return (xz / uNearExtent + 0.5) * ((uCoarseSize - 1.0) / uCoarseSize) + 0.5 / uCoarseSize;
}

float roughnessOf(vec3 masks) {
  float soil = max(0.0, 1.0 - masks.r - masks.g - masks.b);
  return masks.r * ${f(r.rock)} + soil * ${f(r.soil)} + masks.b * ${f(r.glacier)} + masks.g * ${f(r.snow)};
}

// Each octave fades out where the mesh is too coarse to carry it, so distant terrain never aliases.
vec4 detailWeights(float dist) {
  vec4 fadeEnd = vec4(${o.map((x) => f(x.wavelength)).join(', ')}) * uGridQuads / 12.0;
  return 1.0 - smoothstep(fadeEnd * 0.5, fadeEnd, vec4(dist));
}

// Shading can carry an octave much farther than geometry can: until it is smaller than a pixel.
vec4 detailShadeWeights(float dist) {
  vec4 wavelength = vec4(${o.map((x) => f(x.wavelength)).join(', ')});
  return 1.0 - smoothstep(wavelength * 40.0, wavelength * 160.0, vec4(dist));
}

float detailNoise(vec2 xz, vec4 w) {
  float sum = 0.0;
${o.map((x, i) => `  if (w[${i}] > 0.0) sum += w[${i}] * ${f(x.amplitude)} * (valueNoise(xz / ${f(x.wavelength)} + vec2(${f(x.ox)}, ${f(x.oz)})) - 0.5);`).join('\n')}
  return sum;
}

float terrainHeight(vec2 xz, vec3 eye) {
  float wNear = nearWeight(xz);
  float h = baseHeight(xz, wNear);
  if (wNear > 0.0) {
    vec4 w = detailWeights(distance(vec3(xz.x, h, xz.y), eye)) * wNear;
    if (w.x + w.y + w.z + w.w > 0.0) {
      vec3 masks = textureLod(uMasks, coarseUv(xz), 0.0).rgb;
      h += roughnessOf(masks) * detailNoise(xz, w);
    }
  }
  return h;
}
`;

export const HORIZON = /* glsl */ `
uniform sampler2D uHorizon0;
uniform sampler2D uHorizon1;
uniform sampler2D uHorizon2;
uniform sampler2D uHorizon3;

float horizonBin(int bin, vec2 uv) {
  int tex = bin / 3;
  int channel = bin - tex * 3;
  vec3 v;
  if (tex == 0) v = textureLod(uHorizon0, uv, 0.0).rgb;
  else if (tex == 1) v = textureLod(uHorizon1, uv, 0.0).rgb;
  else if (tex == 2) v = textureLod(uHorizon2, uv, 0.0).rgb;
  else v = textureLod(uHorizon3, uv, 0.0).rgb;
  return (channel == 0 ? v.r : (channel == 1 ? v.g : v.b)) * 63.75;
}

// Soft terrain shadow: is the sun above the horizon that this point sees in the sun's direction?
float sunVisibility(vec2 uv) {
  float horizon = mix(horizonBin(uSunBinA, uv), horizonBin(uSunBinB, uv), uSunBinT);
  return smoothstep(-0.8, 0.8, uSunElevDeg - horizon);
}
`;

export const TERRAIN_VERTEX = /* glsl */ `
${ENV_UNIFORMS}
${NOISE}
${TERRAIN_HEIGHT}
uniform vec2 uCenter;
uniform float uScale;
varying vec3 vWorld;
varying vec3 vGeoNormal;
varying float vNearW;

void main() {
  vec2 grid = position.xz;
  vec2 xz = uCenter + grid * uScale;
  vec3 eye = cameraPosition;

  // Blend toward the next coarser level across the outer part of this level, so levels meet
  // without cracks or popping (geometry clipmaps, Losasso and Hoppe 2004).
  float halfExtent = 0.5 * uGridQuads * uScale;
  vec2 d = abs(xz - eye.xz) / halfExtent;
  float morph = clamp((max(d.x, d.y) - 0.70) / 0.25, 0.0, 1.0);
  vec2 odd = mod(grid, 2.0);
  float h = terrainHeight(xz, eye);
  if (morph > 0.0 && odd.x + odd.y > 0.0) {
    float coarse = 0.5 * (terrainHeight(xz - odd * uScale, eye) + terrainHeight(xz + odd * uScale, eye));
    h = mix(h, coarse, morph);
  }

  float wNear = nearWeight(xz);
  vNearW = wNear;
  vGeoNormal = vec3(0.0, 1.0, 0.0);
  if (wNear < 1.0) {
    // Outside the detailed map there is no normal texture: derive normals from the backdrop.
    float e = max(uScale, 80.0);
    float hx = baseHeight(xz + vec2(e, 0.0), nearWeight(xz + vec2(e, 0.0))) - baseHeight(xz - vec2(e, 0.0), nearWeight(xz - vec2(e, 0.0)));
    float hz = baseHeight(xz + vec2(0.0, e), nearWeight(xz + vec2(0.0, e))) - baseHeight(xz - vec2(0.0, e), nearWeight(xz - vec2(0.0, e)));
    vGeoNormal = normalize(vec3(-hx, 2.0 * e, -hz));
  }

  vWorld = vec3(xz.x, h, xz.y);
  gl_Position = projectionMatrix * viewMatrix * vec4(vWorld, 1.0);
}
`;

export const TERRAIN_FRAGMENT = /* glsl */ `
${ENV_UNIFORMS}
${NOISE}
${TERRAIN_HEIGHT}
${HORIZON}
${FOG}
uniform sampler2D uNormal;
uniform sampler2D uAux;
uniform sampler2D uNoise;
uniform vec4 uHole;
uniform float uDetailShading;
varying vec3 vWorld;
varying vec3 vGeoNormal;
varying float vNearW;

void main() {
  vec2 xz = vWorld.xz;
  // The finer level covers this region.
  if (xz.x > uHole.x && xz.x < uHole.z && xz.y > uHole.y && xz.y < uHole.w) discard;
  if (max(abs(xz.x), abs(xz.y)) > uFarExtent * 0.5) discard;

  float y = vWorld.y;
  float dist = distance(vWorld, cameraPosition);
  vec2 uv = coarseUv(xz);
  float nearW = vNearW;

  // Three scales of tileable noise, rotated against each other so the tiling never lines up:
  // kilometre-scale patches, tens of metres, and surface grain.
  vec3 n1 = texture(uNoise, xz * 0.00023).rgb;
  vec4 n2 = texture(uNoise, mat2(0.80, -0.60, 0.60, 0.80) * xz * 0.0027);
  vec4 n3 = texture(uNoise, mat2(0.28, 0.96, -0.96, 0.28) * xz * 0.052);

  // ---- surface normal: exported base normal, plus detail noise, plus fine grain ----
  vec3 N = normalize(vGeoNormal);
  vec3 masks = vec3(0.0);
  float sky = 0.85;
  float shadow = 1.0;
  if (nearW > 0.001) {
    vec2 nuv = (xz / uNearExtent + 0.5) * ((uNearSize - 1.0) / uNearSize) + 0.5 / uNearSize;
    vec3 nTex = texture(uNormal, nuv).xyz * 2.0 - 1.0;
    N = normalize(mix(N, nTex, nearW));
    masks = texture(uMasks, uv).rgb;
    sky = mix(sky, texture(uAux, uv).r, nearW);
    shadow = mix(1.0, sunVisibility(uv), nearW);
  }
  vec2 gradient = -N.xz / max(N.y, 0.05);
  vec4 w3 = detailShadeWeights(dist) * nearW * uDetailShading;
  if (w3.x + w3.y + w3.z + w3.w > 0.001) {
    float e = 0.3 + dist * 0.004;
    float d0 = detailNoise(xz, w3);
    float dx = detailNoise(xz + vec2(e, 0.0), w3);
    float dz = detailNoise(xz + vec2(0.0, e), w3);
    gradient += roughnessOf(masks) * vec2(dx - d0, dz - d0) / e;
  }
  float grain = 1.0 - smoothstep(30.0, 140.0, dist);
  gradient += (n3.rg - 0.5) * 0.55 * grain * (0.25 + masks.r + 0.6 * max(0.0, 1.0 - masks.r - masks.g - masks.b));
  // Mid-distance crags: the 30 m source data is smoother than real rock.
  float craggy = masks.r + 0.35 * max(0.0, 1.0 - masks.r - masks.g - masks.b) + (1.0 - nearW) * 0.5;
  gradient += (n2.rg - 0.5) * (0.10 + 0.75 * craggy) * (1.0 - smoothstep(2500.0, 14000.0, dist));
  N = normalize(vec3(-gradient.x, 1.0, -gradient.y));
  float steep = 1.0 - N.y;

  // ---- what the ground is made of ----
  float rock = masks.r;
  float snow = masks.g;
  float glacier = masks.b;
  if (nearW < 0.999) {
    // Backdrop: no masks, so estimate snow cover from altitude and steepness.
    float farSnow = smoothstep(5350.0, 5900.0, y + (n1.r - 0.5) * 500.0) * (1.0 - smoothstep(0.30, 0.52, steep));
    snow = mix(farSnow, snow, nearW);
    rock = mix((1.0 - farSnow) * smoothstep(0.12, 0.35, steep), rock, nearW);
    glacier *= nearW;
  }
  float soil = max(0.0, 1.0 - rock - snow - glacier);
  // Fresh snowfall dusts whatever is flat enough to hold it; steep facets shed snow and show rock.
  float holds = 1.0 - smoothstep(0.38, 0.70, steep + (n2.b - 0.5) * 0.2);
  float snowCover = clamp(snow + uFreshSnow * holds * (0.55 + 0.9 * n2.a), 0.0, 1.0);
  float shed = smoothstep(0.50, 0.80, steep + (n2.r - 0.5) * 0.28);
  snowCover *= 1.0 - 0.9 * shed;
  float bare = 1.0 - snowCover;
  float underRock = rock + max(0.0, snow - 0.02) * 0.9 + 0.001;
  float under = underRock + glacier + soil;
  float wRock = bare * underRock / under;
  float wIce = bare * glacier / under;
  float wSoil = bare * soil / under;

  // Rock: banded strata, with Everest's Yellow Band of tan limestone near 8,200-8,500 m.
  float strata = texture(uNoise, vec2(vWorld.x * 0.00065 + vWorld.z * 0.00031, y * 0.0043)).r;
  vec3 rockColor = mix(vec3(0.060, 0.055, 0.054), vec3(0.215, 0.195, 0.178), smoothstep(0.2, 0.8, strata * 0.65 + n2.g * 0.5));
  float band = smoothstep(8130.0, 8250.0, y + (n1.b - 0.5) * 180.0) * (1.0 - smoothstep(8420.0, 8540.0, y + (n1.g - 0.5) * 180.0));
  rockColor = mix(rockColor, vec3(0.36, 0.27, 0.15), band * 0.7);
  rockColor *= 0.72 + 0.56 * n3.b;

  vec3 snowColor = vec3(0.80, 0.84, 0.90) * (0.94 + 0.10 * n2.a);

  // Glacier ice: blue where bare, with crevasses opening where the ice bends.
  float crevasse = 1.0 - smoothstep(0.0, 0.035, abs(n2.b - 0.5 + (n1.r - 0.5) * 0.35));
  crevasse *= smoothstep(0.35, 0.6, n1.g) * (1.0 - smoothstep(600.0, 2500.0, dist));
  vec3 iceColor = mix(vec3(0.42, 0.60, 0.72), vec3(0.74, 0.83, 0.89), n2.r);
  iceColor = mix(iceColor, vec3(0.035, 0.12, 0.20), crevasse * 0.85);

  vec3 soilColor = mix(vec3(0.115, 0.098, 0.082), vec3(0.235, 0.21, 0.18), n2.g) * (0.8 + 0.4 * n3.r);
  float meadow = (1.0 - smoothstep(4750.0, 5150.0, y + (n1.r - 0.5) * 300.0)) * holds;
  soilColor = mix(soilColor, vec3(0.085, 0.12, 0.045) * (0.7 + 0.6 * n3.g), meadow * 0.75);

  vec3 albedo = snowColor * snowCover + rockColor * wRock + iceColor * wIce + soilColor * wSoil;

  // ---- light ----
  float nl = dot(N, uSunDir);
  float lit = max(nl, 0.0);
  // Snow scatters light inside itself, which softens the edge between sun and shade.
  lit = mix(lit, clamp((nl + 0.22) / 1.22, 0.0, 1.0), snowCover * 0.55);
  vec3 direct = uSunColor * lit * shadow;
  vec3 ambient = mix(uAmbientGround, uAmbientSky, N.y * 0.5 + 0.5) * (0.25 + 0.75 * sky) * 0.60;
  vec3 moon = uMoonColor * max(dot(N, uMoonDir), 0.0) * (0.4 + 0.6 * sky);
  vec3 color = albedo * (direct + ambient + moon);

  vec3 V = normalize(cameraPosition - vWorld);
  vec3 H = normalize(V + uSunDir);
  float nh = max(dot(N, H), 0.0);
  float gloss = pow(nh, mix(48.0, 260.0, wIce)) * (0.10 * snowCover + 0.9 * wIce);
  float sparkle = step(0.90, n3.a) * pow(nh, 9.0) * snowCover * (1.0 - smoothstep(8.0, 60.0, dist));
  color += uSunColor * shadow * (gloss + sparkle * 2.2);

  if (uLampOn > 0.0) {
    vec3 L = uLampPos - vWorld;
    float d = length(L);
    L /= d;
    float cone = smoothstep(0.72, 0.95, dot(-L, uLampDir));
    color += albedo * vec3(1.0, 0.94, 0.82) * (uLampOn * cone * max(dot(N, L), 0.0) * 22.0 / (1.0 + 0.035 * d * d));
  }

  color = applyFog(color, vWorld, cameraPosition);
  gl_FragColor = vec4(color, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}
`;

export const SKY_VERTEX = /* glsl */ `
varying vec3 vDir;
void main() {
  vDir = position;
  vec4 p = projectionMatrix * viewMatrix * vec4(position * 1000.0 + cameraPosition, 1.0);
  gl_Position = p.xyww;
}
`;

export const SKY_FRAGMENT = /* glsl */ `
${ENV_UNIFORMS}
uniform sampler2D uNoise;
varying vec3 vDir;

float hash13(vec3 p) {
  p = fract(p * 0.1031);
  p += dot(p, p.zyx + 31.32);
  return fract((p.x + p.y) * p.z);
}

void main() {
  vec3 dir = normalize(vDir);
  float up = dir.y;
  float mu = dot(dir, uSunDir);

  // Gradient from zenith to horizon, warmed toward the sun at dawn and dusk.
  float t = pow(1.0 - clamp(up, 0.0, 1.0), 3.4);
  vec3 horizon = uSkyHorizon;
  vec2 dh = normalize(dir.xz + 1e-5);
  vec2 sh = normalize(uSunDir.xz + 1e-5);
  float sunSide = pow(max(dot(dh, sh), 0.0), 2.5);
  horizon = mix(horizon, uSunsetColor * (0.25 + 0.75 * length(uSkyHorizon) + 0.35), uTwilight * sunSide * 0.8);
  vec3 color = mix(uSkyZenith, horizon, t);
  if (up < 0.0) color = mix(horizon, uFogColor, smoothstep(0.0, -0.12, up));

  // Sun: forward-scattering glow and the disc itself.
  float glow = 0.045 * pow(max(mu, 0.0), 6.0) + 0.55 * pow(max(mu, 0.0), 420.0);
  color += uSunColor * glow * (1.0 - 0.7 * uStorm);
  float disc = smoothstep(0.99993, 0.99997, mu);
  color += uSunColor * disc * 18.0 * smoothstep(-0.02, 0.01, up);

  // Moon.
  float moonMu = dot(dir, uMoonDir);
  float moonDisc = smoothstep(0.99990, 0.99995, moonMu);
  float moonLight = max(max(uMoonColor.r, uMoonColor.g), uMoonColor.b);
  color += vec3(0.9, 0.93, 1.0) * moonDisc * (0.25 + 6.0 * moonLight) * step(0.0, uMoonDir.y);
  color += uMoonColor * 0.6 * pow(max(moonMu, 0.0), 24.0);

  // Stars: thin air at altitude makes them sharp and many.
  if (uNight > 0.01 && up > -0.05) {
    vec3 p = dir * 260.0;
    vec3 cell = floor(p);
    float h = hash13(cell);
    float star = step(0.986, h) * smoothstep(0.42, 0.0, length(fract(p) - 0.5));
    float twinkle = 0.75 + 0.25 * sin(uTime * (1.5 + 4.0 * fract(h * 57.0)) + h * 80.0);
    vec3 tint = mix(vec3(1.0, 0.86, 0.72), vec3(0.75, 0.86, 1.0), fract(h * 13.0));
    color += tint * star * twinkle * uNight * (0.35 + 1.4 * fract(h * 7.0)) * smoothstep(-0.05, 0.12, up);
    // The Milky Way as a faint band.
    float bandDist = abs(dot(dir, normalize(vec3(0.42, 0.33, -0.85))));
    color += vec3(0.018, 0.021, 0.03) * uNight * smoothstep(0.35, 0.0, bandDist) * texture(uNoise, dir.xz * 1.7 + dir.y).r;
  }

  // High cloud drifting on the wind.
  if (up > 0.015) {
    vec2 plane = dir.xz / (up + 0.10);
    vec2 drift = uWind * uTime * 0.0006;
    float c = texture(uNoise, plane * 0.045 + drift).r * 0.62 + texture(uNoise, plane * 0.13 + drift * 1.7).g * 0.38;
    float cover = clamp(uCloudCover * 0.95 + uStorm * 0.5, 0.0, 1.0);
    float density = smoothstep(1.0 - cover * 0.95, 1.15 - cover * 0.75, c + 0.18);
    density *= smoothstep(0.015, 0.14, up);
    float sunLit = 0.55 + 0.45 * pow(max(mu, 0.0), 3.0);
    vec3 cloudColor = uSunColor * 0.32 * sunLit + uAmbientSky * 0.85 + uMoonColor * 0.5;
    cloudColor = mix(cloudColor, cloudColor * 0.35, uStorm * 0.8);
    color = mix(color, cloudColor, density * 0.92);
  }

  gl_FragColor = vec4(color, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}
`;

// The valley cloud layer: a sheet of cloud that fills the valleys and fades where terrain rises
// through it, so from high camps the peaks stand out of a cloud sea.
export const CLOUD_VERTEX = /* glsl */ `
varying vec3 vWorld;
void main() {
  vec4 world = modelMatrix * vec4(position, 1.0);
  vWorld = world.xyz;
  gl_Position = projectionMatrix * viewMatrix * world;
}
`;

export const CLOUD_FRAGMENT = /* glsl */ `
${ENV_UNIFORMS}
${NOISE}
${TERRAIN_HEIGHT}
${FOG}
uniform sampler2D uNoise;
uniform float uLayer;
varying vec3 vWorld;

void main() {
  vec2 xz = vWorld.xz;
  float wNear = nearWeight(xz);
  float ground = baseHeight(xz, wNear);
  float above = vWorld.y - ground;
  if (above < 2.0) discard;
  vec2 drift = uWind * uTime * 0.02;
  vec2 p = xz + drift * (1.0 + uLayer * 0.25);
  float n = texture(uNoise, p * 0.00009).r * 0.55 + texture(uNoise, p * 0.00031 + uLayer * 0.37).g * 0.30
          + texture(uNoise, p * 0.0011).b * 0.15;
  float cover = clamp(uCloudCover * 1.05 + uStorm * 0.35, 0.0, 1.0);
  float density = smoothstep(1.02 - cover, 1.22 - cover * 0.8, n + 0.2);
  density *= smoothstep(2.0, 160.0, above);
  float dist = distance(vWorld, cameraPosition);
  density *= smoothstep(30.0, 500.0, dist);
  if (density < 0.004) discard;
  float towardSun = max(dot(normalize(vWorld - cameraPosition), uSunDir), 0.0);
  vec3 color = uSunColor * (0.30 + 0.10 * uLayer + 0.25 * pow(towardSun, 4.0)) * max(uSunDir.y + 0.15, 0.0)
             + uAmbientSky * 0.80 + uMoonColor * 0.6;
  color *= mix(1.0, 0.45, uStorm);
  color = applyFog(color, vWorld, cameraPosition);
  gl_FragColor = vec4(color, density * 0.9);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}
`;
