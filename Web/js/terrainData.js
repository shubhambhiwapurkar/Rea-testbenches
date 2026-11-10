// Terrain on the CPU side: decodes the exported maps and answers "what is the ground like here?"
// for movement, the simulation and object placement. The detail noise here is the same integer-hash
// function the terrain shader uses, so the ground the climber stands on is the ground that is drawn.

export const DETAIL = Object.freeze({
  // wavelength (m), amplitude (m at roughness 1), offset
  octaves: [
    { wavelength: 61.0, amplitude: 6.0, ox: 3.7, oz: 11.9 },
    { wavelength: 23.0, amplitude: 1.5, ox: 0.0, oz: 0.0 },
    { wavelength: 7.3, amplitude: 1.7, ox: 17.1, oz: -9.4 },
    { wavelength: 3.1, amplitude: 0.55, ox: -5.7, oz: 3.3 },
  ],
  roughness: { rock: 1.0, soil: 0.55, glacier: 0.5, snow: 0.14 },
});

async function loadPixels(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`Could not load ${url} (${response.status})`);
  const bitmap = await createImageBitmap(await response.blob(), { colorSpaceConversion: 'none', premultiplyAlpha: 'none' });
  const canvas = document.createElement('canvas');
  canvas.width = bitmap.width;
  canvas.height = bitmap.height;
  const ctx = canvas.getContext('2d', { willReadFrequently: true, colorSpace: 'srgb' });
  ctx.drawImage(bitmap, 0, 0);
  const image = ctx.getImageData(0, 0, bitmap.width, bitmap.height);
  bitmap.close?.();
  return { width: image.width, height: image.height, data: image.data };
}

function decodeHeights(pixels, minM, maxM) {
  const n = pixels.width * pixels.height;
  const out = new Float32Array(n);
  const scale = (maxM - minM) / 65535;
  const d = pixels.data;
  for (let i = 0; i < n; i++) out[i] = minM + ((d[i * 4] << 8) | d[i * 4 + 1]) * scale;
  return out;
}

export function hash2(ix, iz) {
  let h = (Math.imul(ix, 0x9E3779B1) ^ Math.imul(iz, 0x85EBCA77)) >>> 0;
  h ^= h >>> 15;
  h = Math.imul(h, 0x2C1B3C6D) >>> 0;
  h ^= h >>> 12;
  h = Math.imul(h, 0x297A2D39) >>> 0;
  h ^= h >>> 15;
  return (h >>> 0) / 4294967296;
}

export function valueNoise(x, z) {
  const ix = Math.floor(x);
  const iz = Math.floor(z);
  const fx = x - ix;
  const fz = z - iz;
  const ux = fx * fx * (3 - 2 * fx);
  const uz = fz * fz * (3 - 2 * fz);
  const a = hash2(ix, iz);
  const b = hash2(ix + 1, iz);
  const c = hash2(ix, iz + 1);
  const d = hash2(ix + 1, iz + 1);
  return a + (b - a) * ux + (c - a) * uz + (a - b - c + d) * ux * uz;
}

export class TerrainData {
  static async load(base, world, onProgress = () => {}) {
    const files = [world.near.file, world.far.file, world.masks.file, world.aux.file, ...world.horizon.files];
    let done = 0;
    const pixels = await Promise.all(files.map(async (file) => {
      const p = await loadPixels(base + file);
      onProgress(++done / files.length);
      return p;
    }));
    return new TerrainData(world, pixels);
  }

  constructor(world, pixels) {
    const [near, far, masks, aux, ...horizons] = pixels;
    this.world = world;
    this.size = world.near.size;
    this.extent = world.near.extent_m;
    this.half = this.extent / 2;
    this.cell = this.extent / (this.size - 1);
    this.h = decodeHeights(near, world.near.min_m, world.near.max_m);
    this.minM = world.near.min_m;
    this.maxM = world.near.max_m;

    this.farSize = world.far.size;
    this.farExtent = world.far.extent_m;
    this.farH = decodeHeights(far, world.far.min_m, world.far.max_m);

    this.coarse = world.masks.size;
    this.masks = masks.data;   // RGBA: rock, snow, glacier
    this.aux = aux.data;       // RGBA: sky visibility, wind exposure
    this.horizonDirections = world.horizon.directions;
    this.horizon = [];
    for (let d = 0; d < this.horizonDirections; d++) {
      const src = horizons[Math.floor(d / 3)].data;
      const channel = d % 3;
      const out = new Uint8Array(this.coarse * this.coarse);
      for (let i = 0; i < out.length; i++) out[i] = src[i * 4 + channel];
      this.horizon.push(out);
    }
    this.horizonPixels = horizons;
    // Playable bounds: keep the climber off the blended edge of the map.
    this.bound = this.half - 350;
    this._m = { rock: 0, snow: 0, glacier: 0, soil: 0 };
  }

  inside(x, z) { return Math.abs(x) <= this.bound && Math.abs(z) <= this.bound; }

  // Bilinear height of the exported map, without detail.
  baseHeight(x, z) {
    const n = this.size;
    let fx = (x + this.half) / this.cell;
    let fz = (z + this.half) / this.cell;
    fx = fx < 0 ? 0 : fx > n - 1.001 ? n - 1.001 : fx;
    fz = fz < 0 ? 0 : fz > n - 1.001 ? n - 1.001 : fz;
    const ix = fx | 0;
    const iz = fz | 0;
    const tx = fx - ix;
    const tz = fz - iz;
    const i = iz * n + ix;
    const h = this.h;
    const top = h[i] + (h[i + 1] - h[i]) * tx;
    const bottom = h[i + n] + (h[i + n + 1] - h[i + n]) * tx;
    return top + (bottom - top) * tz;
  }

  _coarse(data, channel, x, z, stride) {
    const n = this.coarse;
    let fx = (x + this.half) / this.extent * (n - 1);
    let fz = (z + this.half) / this.extent * (n - 1);
    fx = fx < 0 ? 0 : fx > n - 1.001 ? n - 1.001 : fx;
    fz = fz < 0 ? 0 : fz > n - 1.001 ? n - 1.001 : fz;
    const ix = fx | 0;
    const iz = fz | 0;
    const tx = fx - ix;
    const tz = fz - iz;
    const i = (iz * n + ix) * stride + channel;
    const a = data[i];
    const b = data[i + stride];
    const c = data[i + n * stride];
    const d = data[i + n * stride + stride];
    return (a + (b - a) * tx + (c - a) * tz + (a - b - c + d) * tx * tz) / 255;
  }

  // Surface mix at a point. Returns a shared object: read it before the next call.
  materialAt(x, z) {
    const m = this._m;
    m.rock = this._coarse(this.masks, 0, x, z, 4);
    m.snow = this._coarse(this.masks, 1, x, z, 4);
    m.glacier = this._coarse(this.masks, 2, x, z, 4);
    m.soil = Math.max(0, 1 - m.rock - m.snow - m.glacier);
    return m;
  }

  skyVisibility(x, z) { return this._coarse(this.aux, 0, x, z, 4); }
  windExposure(x, z) { return this._coarse(this.aux, 1, x, z, 4); }

  detail(x, z) {
    const m = this.materialAt(x, z);
    const r = DETAIL.roughness;
    const rough = m.rock * r.rock + m.soil * r.soil + m.glacier * r.glacier + m.snow * r.snow;
    let sum = 0;
    for (const o of DETAIL.octaves) {
      sum += o.amplitude * (valueNoise(x / o.wavelength + o.ox, z / o.wavelength + o.oz) - 0.5);
    }
    return rough * sum;
  }

  height(x, z) { return this.baseHeight(x, z) + this.detail(x, z); }

  // Surface normal including detail. out = [nx, ny, nz].
  normalAt(x, z, out, eps = 0.75) {
    const dx = this.height(x + eps, z) - this.height(x - eps, z);
    const dz = this.height(x, z + eps) - this.height(x, z - eps);
    const nx = -dx;
    const ny = 2 * eps;
    const nz = -dz;
    const l = Math.hypot(nx, ny, nz);
    out[0] = nx / l;
    out[1] = ny / l;
    out[2] = nz / l;
    return out;
  }

  // Slope of the broad terrain (10 m scale), which decides avalanche and sliding behaviour.
  baseSlopeDeg(x, z, eps = 8) {
    const dx = (this.baseHeight(x + eps, z) - this.baseHeight(x - eps, z)) / (2 * eps);
    const dz = (this.baseHeight(x, z + eps) - this.baseHeight(x, z - eps)) / (2 * eps);
    return Math.atan(Math.hypot(dx, dz)) * 180 / Math.PI;
  }

  // Downhill direction of the broad terrain as a unit vector [x, z], and its compass aspect.
  fallLine(x, z, out, eps = 8) {
    const dx = this.baseHeight(x + eps, z) - this.baseHeight(x - eps, z);
    const dz = this.baseHeight(x, z + eps) - this.baseHeight(x, z - eps);
    const l = Math.hypot(dx, dz) || 1;
    out[0] = -dx / l;
    out[1] = -dz / l;
    return out;
  }

  // Horizon elevation (deg) looking along a compass azimuth (deg clockwise from north).
  horizonDeg(x, z, azimuthDeg) {
    const n = this.horizonDirections;
    const f = ((azimuthDeg % 360) + 360) % 360 / (360 / n);
    const a = Math.floor(f) % n;
    const b = (a + 1) % n;
    const t = f - Math.floor(f);
    const ha = this._coarse(this.horizon[a], 0, x, z, 1) * 255 * 0.25;
    const hb = this._coarse(this.horizon[b], 0, x, z, 1) * 255 * 0.25;
    return ha + (hb - ha) * t;
  }

  // 0 in terrain shadow, 1 in full sun, with a soft edge like the shader's.
  sunVisibility(x, z, sunElevationDeg, sunAzimuthDeg) {
    if (sunElevationDeg <= -1) return 0;
    const t = (sunElevationDeg - this.horizonDeg(x, z, sunAzimuthDeg) + 0.8) / 1.6;
    return t <= 0 ? 0 : t >= 1 ? 1 : t * t * (3 - 2 * t);
  }

  // Base normals packed for the shader (RGB = normal * 0.5 + 0.5).
  buildNormalBytes() {
    const n = this.size;
    const out = new Uint8Array(n * n * 4);
    const h = this.h;
    const inv = 1 / (2 * this.cell);
    for (let z = 0; z < n; z++) {
      const zu = z > 0 ? z - 1 : z;
      const zd = z < n - 1 ? z + 1 : z;
      for (let x = 0; x < n; x++) {
        const xl = x > 0 ? x - 1 : x;
        const xr = x < n - 1 ? x + 1 : x;
        const dx = (h[z * n + xr] - h[z * n + xl]) * inv;
        const dz = (h[zd * n + x] - h[zu * n + x]) * inv;
        const l = 1 / Math.hypot(dx, 1, dz);
        const o = (z * n + x) * 4;
        out[o] = (-dx * l * 0.5 + 0.5) * 255;
        out[o + 1] = (l * 0.5 + 0.5) * 255;
        out[o + 2] = (-dz * l * 0.5 + 0.5) * 255;
        out[o + 3] = 255;
      }
    }
    return out;
  }
}
