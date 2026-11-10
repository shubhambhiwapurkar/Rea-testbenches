// Falling snow and spindrift. Both are one cloud of points that wraps around the camera; the vertex
// shader moves them with the wind, so the wind you see is the wind the simulation is applying.

import * as THREE from '../vendor/three.module.min.js';
import { ENV_UNIFORMS, NOISE, TERRAIN_HEIGHT } from './shaders.js';

const VERTEX = /* glsl */ `
${ENV_UNIFORMS}
${NOISE}
${TERRAIN_HEIGHT}
attribute vec3 seed;
uniform float uBox;
uniform float uAmount;
uniform float uGroundHugging;
uniform float uPixelScale;
varying float vAlpha;

void main() {
  float t = uTime;
  vec3 velocity = vec3(uWind.x, -1.1 - 1.4 * seed.z, uWind.y);
  if (uGroundHugging > 0.5) velocity.y = 0.0;
  // Each particle has a home in a repeating box that follows the camera.
  vec3 p = seed * uBox + velocity * t;
  p.xz += vec2(sin(t * 1.7 + seed.x * 40.0), cos(t * 1.3 + seed.y * 40.0)) * (0.35 + 0.02 * uWindSpeed);
  vec3 local = mod(p - cameraPosition + 0.5 * uBox, uBox) - 0.5 * uBox;
  vec3 world = cameraPosition + local;
  if (uGroundHugging > 0.5) {
    // Spindrift streams along the surface in a layer a couple of metres deep.
    float ground = baseHeight(world.xz, nearWeight(world.xz));
    float lift = seed.y * seed.y * (0.3 + 0.16 * uWindSpeed);
    world.y = ground + 0.15 + lift + 0.4 * sin(t * 3.0 + seed.x * 30.0) * seed.y;
  }
  vec4 view = viewMatrix * vec4(world, 1.0);
  gl_Position = projectionMatrix * view;
  float dist = -view.z;
  float size = (uGroundHugging > 0.5 ? 0.11 : 0.05 + 0.05 * seed.z) * uPixelScale / max(dist, 0.3);
  gl_PointSize = clamp(size, 0.0, 28.0);
  float edge = 1.0 - smoothstep(0.38, 0.5, max(max(abs(local.x), abs(local.y)), abs(local.z)) / uBox);
  // Fewer particles are shown when there is less snow in the air.
  vAlpha = step(seed.x, uAmount) * edge * smoothstep(0.3, 1.5, dist);
  if (vAlpha <= 0.0) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
}
`;

const FRAGMENT = /* glsl */ `
${ENV_UNIFORMS}
uniform float uGroundHugging;
varying float vAlpha;
void main() {
  vec2 c = gl_PointCoord - 0.5;
  float a = smoothstep(0.5, 0.08, length(c)) * vAlpha;
  if (a < 0.01) discard;
  vec3 light = uSunColor * 0.45 + uAmbientSky * 0.9 + uMoonColor + vec3(1.0, 0.95, 0.85) * uLampOn * 0.25;
  gl_FragColor = vec4(vec3(0.92, 0.95, 1.0) * light, a * (uGroundHugging > 0.5 ? 0.42 : 0.8));
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}
`;

function makeCloud(count, box, groundHugging, shared) {
  const seeds = new Float32Array(count * 3);
  let s = groundHugging ? 12345 : 6789;
  for (let i = 0; i < seeds.length; i++) {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    seeds[i] = s / 4294967296;
  }
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute('position', new THREE.BufferAttribute(new Float32Array(count * 3), 3));
  geometry.setAttribute('seed', new THREE.BufferAttribute(seeds, 3));
  const material = new THREE.ShaderMaterial({
    uniforms: {
      ...shared,
      uBox: { value: box },
      uAmount: { value: 0 },
      uGroundHugging: { value: groundHugging ? 1 : 0 },
      uPixelScale: { value: 900 },
    },
    vertexShader: VERTEX,
    fragmentShader: FRAGMENT,
    transparent: true,
    depthWrite: false,
  });
  const points = new THREE.Points(geometry, material);
  points.frustumCulled = false;
  points.renderOrder = 80;
  return points;
}

export class WeatherParticles {
  constructor(terrainUniforms, { count = 9000 } = {}) {
    this.group = new THREE.Group();
    this.snow = makeCloud(count, 46, false, terrainUniforms);
    this.drift = makeCloud(Math.floor(count * 0.7), 70, true, terrainUniforms);
    this.group.add(this.snow, this.drift);
    this.snowAmount = 0;
    this.driftAmount = 0;
  }

  // snowfallCmH from the sim; windMS at the player; snowUnderfoot 0..1 (is there loose snow to move?).
  update(dt, snowfallCmH, windMS, snowUnderfoot, viewportHeight, fovDeg) {
    const snowTarget = Math.min(1, snowfallCmH / 1.6);
    // Snow starts to drift at roughly 7-8 m/s and is a ground blizzard by 20 m/s.
    const driftTarget = Math.min(1, Math.max(0, (windMS - 7) / 14)) * (0.25 + 0.75 * snowUnderfoot) + snowTarget * 0.3;
    const k = Math.min(1, dt * 0.8);
    this.snowAmount += (snowTarget - this.snowAmount) * k;
    this.driftAmount += (Math.min(1, driftTarget) - this.driftAmount) * k;
    const pixelScale = viewportHeight / (2 * Math.tan((fovDeg * Math.PI) / 360));
    for (const cloud of [this.snow, this.drift]) cloud.material.uniforms.uPixelScale.value = pixelScale;
    this.snow.material.uniforms.uAmount.value = this.snowAmount;
    this.drift.material.uniforms.uAmount.value = this.driftAmount;
    this.snow.visible = this.snowAmount > 0.005;
    this.drift.visible = this.driftAmount > 0.005;
  }
}
