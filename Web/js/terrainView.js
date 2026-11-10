// Draws the terrain as a geometry clipmap: nested square grids centred on the camera, each twice as
// coarse as the one inside it. One small grid mesh is reused for every level; the vertex shader reads
// heights from textures, so the 80 km world costs a few hundred thousand triangles at any position.

import * as THREE from '../vendor/three.module.min.js';
import { TERRAIN_FRAGMENT, TERRAIN_VERTEX } from './shaders.js';

function floatTexture(data, size) {
  const t = new THREE.DataTexture(data, size, size, THREE.RedFormat, THREE.FloatType);
  t.minFilter = t.magFilter = THREE.NearestFilter;
  t.needsUpdate = true;
  return t;
}

function byteTexture(data, width, height, { mipmaps = false, repeat = false } = {}) {
  const t = new THREE.DataTexture(data, width, height, THREE.RGBAFormat, THREE.UnsignedByteType);
  t.magFilter = THREE.LinearFilter;
  t.minFilter = mipmaps ? THREE.LinearMipmapLinearFilter : THREE.LinearFilter;
  t.generateMipmaps = mipmaps;
  if (repeat) t.wrapS = t.wrapT = THREE.RepeatWrapping;
  t.colorSpace = THREE.NoColorSpace;
  t.needsUpdate = true;
  return t;
}

// Four channels of tileable noise the shaders use for colour variation, grain and clouds.
export function buildNoiseTexture(size = 256) {
  const data = new Uint8Array(size * size * 4);
  const lattice = (period, seed) => {
    const g = new Float32Array(period * period);
    let s = seed >>> 0;
    for (let i = 0; i < g.length; i++) {
      s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
      g[i] = s / 4294967296;
    }
    return (x, y) => {
      const xi = Math.floor(x), yi = Math.floor(y);
      const fx = x - xi, fy = y - yi;
      const ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
      const x0 = ((xi % period) + period) % period, x1 = (x0 + 1) % period;
      const y0 = ((yi % period) + period) % period, y1 = (y0 + 1) % period;
      const a = g[y0 * period + x0], b = g[y0 * period + x1], c = g[y1 * period + x0], d = g[y1 * period + x1];
      return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
    };
  };
  const fbm = (base, seed, ridged) => {
    const octaves = [];
    for (let o = 0; o < 5; o++) octaves.push(lattice(base << o, seed + o * 101));
    return (u, v) => {
      let sum = 0, amp = 0.5, norm = 0;
      for (let o = 0; o < 5; o++) {
        let n = octaves[o](u * (base << o), v * (base << o));
        if (ridged) n = 1 - Math.abs(n * 2 - 1);
        sum += n * amp;
        norm += amp;
        amp *= 0.5;
      }
      return sum / norm;
    };
  };
  const channels = [fbm(4, 11, false), fbm(6, 23, false), fbm(5, 37, true), fbm(32, 53, false)];
  // Stretch each channel to the full range so thresholds in the shaders behave predictably.
  const values = channels.map(() => new Float32Array(size * size));
  channels.forEach((fn, c) => {
    let lo = 1, hi = 0;
    for (let y = 0; y < size; y++) {
      for (let x = 0; x < size; x++) {
        const v = fn(x / size, y / size);
        values[c][y * size + x] = v;
        lo = Math.min(lo, v);
        hi = Math.max(hi, v);
      }
    }
    for (let i = 0; i < size * size; i++) data[i * 4 + c] = ((values[c][i] - lo) / (hi - lo)) * 255;
  });
  return byteTexture(data, size, size, { mipmaps: true, repeat: true });
}

export class TerrainView {
  constructor(data, env, noiseTexture, { quads = 128, finestCellDivisor = 8, detailShading = 1, satellite = null } = {}) {
    this.data = data;
    this.quads = quads;
    this.spacing0 = data.cell / finestCellDivisor;
    this.group = new THREE.Group();

    const coarse = data.coarse;
    this.textures = {
      height: floatTexture(data.h, data.size),
      far: floatTexture(data.farH, data.farSize),
      normal: byteTexture(data.buildNormalBytes(), data.size, data.size, { mipmaps: true }),
      masks: byteTexture(data.masks, coarse, coarse),
      aux: byteTexture(data.aux, coarse, coarse),
      horizon: data.horizonPixels.map((p) => byteTexture(p.data, coarse, coarse)),
    };

    this.shared = {
      ...env,
      uHeight: { value: this.textures.height },
      uFar: { value: this.textures.far },
      uNormal: { value: this.textures.normal },
      uMasks: { value: this.textures.masks },
      uAux: { value: this.textures.aux },
      uNoise: { value: noiseTexture },
      uSatellite: { value: satellite ?? noiseTexture },
      uHasSatellite: { value: satellite ? 1 : 0 },
      uHorizon0: { value: this.textures.horizon[0] },
      uHorizon1: { value: this.textures.horizon[1] },
      uHorizon2: { value: this.textures.horizon[2] },
      uHorizon3: { value: this.textures.horizon[3] },
      uNearSize: { value: data.size },
      uNearExtent: { value: data.extent },
      uFarSize: { value: data.farSize },
      uFarExtent: { value: data.farExtent },
      uCoarseSize: { value: coarse },
      uGridQuads: { value: quads },
      uDetailShading: { value: detailShading },
    };

    const geometry = this.buildGrid(quads);
    this.levels = [];
    const farHalf = data.farExtent / 2;
    for (let level = 0; ; level++) {
      const scale = this.spacing0 * 2 ** level;
      const material = new THREE.ShaderMaterial({
        uniforms: {
          ...this.shared,
          uCenter: { value: new THREE.Vector2() },
          uScale: { value: scale },
          uHole: { value: new THREE.Vector4(1, 1, -1, -1) },
        },
        vertexShader: TERRAIN_VERTEX,
        fragmentShader: TERRAIN_FRAGMENT,
      });
      const mesh = new THREE.Mesh(geometry, material);
      mesh.frustumCulled = false;
      mesh.renderOrder = level;
      mesh.matrixAutoUpdate = false;
      this.group.add(mesh);
      this.levels.push({ mesh, material, scale });
      if (scale * quads / 2 >= farHalf * 1.5) break;
    }
  }

  buildGrid(quads) {
    const n = quads + 1;
    const half = quads / 2;
    const positions = new Float32Array(n * n * 3);
    for (let z = 0; z < n; z++) {
      for (let x = 0; x < n; x++) {
        const i = (z * n + x) * 3;
        positions[i] = x - half;
        positions[i + 2] = z - half;
      }
    }
    const indices = new Uint32Array(quads * quads * 6);
    let k = 0;
    for (let z = 0; z < quads; z++) {
      for (let x = 0; x < quads; x++) {
        const a = z * n + x, b = a + 1, c = a + n, d = c + 1;
        // Both triangles share the a-d diagonal; the vertex shader's morph relies on that.
        indices[k++] = a; indices[k++] = c; indices[k++] = d;
        indices[k++] = a; indices[k++] = d; indices[k++] = b;
      }
    }
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    geometry.setIndex(new THREE.BufferAttribute(indices, 1));
    return geometry;
  }

  update(camera) {
    const half = this.quads / 2;
    let finer = null;
    for (const level of this.levels) {
      // Snap to twice the level's spacing so its even vertices are the next level's vertices.
      const snap = level.scale * 2;
      const cx = Math.round(camera.position.x / snap) * snap;
      const cz = Math.round(camera.position.z / snap) * snap;
      level.material.uniforms.uCenter.value.set(cx, cz);
      const hole = level.material.uniforms.uHole.value;
      if (finer) {
        const r = (half - 1) * finer.scale;
        hole.set(finer.cx - r, finer.cz - r, finer.cx + r, finer.cz + r);
      } else {
        hole.set(1, 1, -1, -1);
      }
      finer = { cx, cz, scale: level.scale };
    }
  }
}
