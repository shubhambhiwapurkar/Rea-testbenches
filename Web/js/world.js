// The climbing route and the things people put on the mountain: camps, wands, fixed ropes,
// ladders and prayer flags.

import * as THREE from '../vendor/three.module.min.js';
import { ENV_UNIFORMS, FOG } from './shaders.js';
import { hash2 } from './terrainData.js';

export class Route {
  constructor(world, data) {
    const r = world.route;
    this.n = r.x.length;
    this.x = Float32Array.from(r.x);
    this.z = Float32Array.from(r.z);
    this.y = new Float32Array(this.n);
    this.s = new Float32Array(this.n);   // distance along the route (m, on the ground)
    this.fixed = Uint8Array.from(r.fixed);
    for (let i = 0; i < this.n; i++) {
      this.y[i] = data.height(this.x[i], this.z[i]);
      if (i > 0) {
        this.s[i] = this.s[i - 1] + Math.hypot(this.x[i] - this.x[i - 1], this.z[i] - this.z[i - 1], this.y[i] - this.y[i - 1]);
      }
    }
    this.length = this.s[this.n - 1];
    this.legs = r.legs;
    this.camps = world.camps.map((c) => ({
      ...c,
      index: c.route_index,
      s: this.s[c.route_index],
      y: data.height(c.x, c.z),
      short: { ebc: 'Base Camp', c1: 'Camp 1', c2: 'Camp 2', c3: 'Camp 3', c4: 'Camp 4', summit: 'Summit' }[c.id] ?? c.name,
    }));
    this.stops = this.camps.filter((c) => c.kind === 'camp');
    this.summit = this.camps[this.camps.length - 1];
    this._near = { index: 0, t: 0, dist: 0, x: 0, z: 0, s: 0 };
  }

  camp(id) { return this.camps.find((c) => c.id === id); }

  // Closest point on the route. Returns a shared object.
  nearest(x, z) {
    let best = Infinity;
    const out = this._near;
    for (let i = 0; i < this.n - 1; i++) {
      const ax = this.x[i], az = this.z[i];
      const bx = this.x[i + 1] - ax, bz = this.z[i + 1] - az;
      const len2 = bx * bx + bz * bz || 1;
      let t = ((x - ax) * bx + (z - az) * bz) / len2;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      const px = ax + bx * t, pz = az + bz * t;
      const d = (x - px) * (x - px) + (z - pz) * (z - pz);
      if (d < best) {
        best = d;
        out.index = i;
        out.t = t;
        out.x = px;
        out.z = pz;
      }
    }
    out.dist = Math.sqrt(best);
    out.s = this.s[out.index] + (this.s[out.index + 1] - this.s[out.index]) * out.t;
    return out;
  }

  // Position at a distance along the route. out = { x, z, index }.
  pointAt(s, out = {}) {
    s = Math.min(Math.max(s, 0), this.length);
    let lo = 0, hi = this.n - 1;
    while (hi - lo > 1) {
      const mid = (lo + hi) >> 1;
      if (this.s[mid] <= s) lo = mid; else hi = mid;
    }
    const t = (s - this.s[lo]) / (this.s[hi] - this.s[lo] || 1);
    out.x = this.x[lo] + (this.x[hi] - this.x[lo]) * t;
    out.z = this.z[lo] + (this.z[hi] - this.z[lo]) * t;
    out.index = lo;
    return out;
  }

  legAt(index) { return this.legs.find((l) => index >= l.i0 && index < l.i1) ?? this.legs[this.legs.length - 1]; }
  isFixed(index) { return this.fixed[Math.min(index, this.n - 1)] === 1; }

  // The camp a climber at distance s is heading for, going up or down.
  nextStop(s, upward) {
    if (upward) return this.stops.find((c) => c.s > s + 15) ?? this.summit;
    for (let i = this.stops.length - 1; i >= 0; i--) if (this.stops[i].s < s - 15) return this.stops[i];
    return this.stops[0];
  }
}

const TENT_COLORS = [0xf0b414, 0xe86a1c, 0xd8392b, 0xf0b414, 0x2e7cc0, 0xe86a1c];
const CAMP_TENTS = { ebc: 11, c1: 4, c2: 7, c3: 3, c4: 4 };
const FLAG_COLORS = [0x2b62c4, 0xf2f2f2, 0xd0342c, 0x2f9e4f, 0xf0c21a];  // sky, wind, fire, water, earth

const FLAG_VERTEX = /* glsl */ `
${ENV_UNIFORMS}
attribute vec3 color;
attribute float flutter;
varying vec3 vColor;
varying vec3 vWorld;
void main() {
  vec3 p = position;
  // Flags stream downwind and snap harder as the wind rises.
  float strength = clamp(uWindSpeed / 18.0, 0.05, 1.0);
  float wave = sin(uTime * (5.0 + 9.0 * strength) + p.x * 3.1 + p.z * 2.3 + flutter * 6.28);
  vec2 downwind = normalize(uWind + vec2(1e-4));
  p.xz += downwind * flutter * (0.10 + 0.34 * strength) * (1.0 + 0.5 * wave);
  p.y += flutter * 0.10 * strength * wave + flutter * 0.16 * strength;
  vColor = color;
  vec4 world = modelMatrix * vec4(p, 1.0);
  vWorld = world.xyz;
  gl_Position = projectionMatrix * viewMatrix * world;
}
`;

const FLAG_FRAGMENT = /* glsl */ `
${ENV_UNIFORMS}
${FOG}
uniform float uShade;
varying vec3 vColor;
varying vec3 vWorld;
void main() {
  vec3 light = uSunColor * (0.55 * uShade) + uAmbientSky * 0.75 + uMoonColor;
  vec3 color = applyFog(vColor * light, vWorld, cameraPosition);
  gl_FragColor = vec4(color, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}
`;

export class WorldObjects {
  constructor(data, route, env) {
    this.data = data;
    this.route = route;
    this.group = new THREE.Group();
    this.flagMaterial = new THREE.ShaderMaterial({
      uniforms: { ...env, uShade: { value: 1 } },
      vertexShader: FLAG_VERTEX,
      fragmentShader: FLAG_FRAGMENT,
      side: THREE.DoubleSide,
    });
    this.flagStrings = [];
    for (const camp of route.stops) {
      if (camp.id === 'summit') this.buildSummit(camp); else this.buildCamp(camp);
    }
    this.buildWands();
    this.buildRopes();
    this.buildLadders();
    if (this.flagStrings.length) this.group.add(this.buildFlagMesh());
  }

  ground(x, z, lift = 0) { return this.data.height(x, z) + lift; }

  buildCamp(camp) {
    const count = CAMP_TENTS[camp.id] ?? 3;
    const dome = new THREE.SphereGeometry(1, 12, 6, 0, Math.PI * 2, 0, Math.PI / 2);
    const normal = [0, 1, 0];
    camp.tents = [];
    for (let i = 0; i < count; i++) {
      // Scatter tents on the flattest nearby ground, as a camp manager would.
      let best = null;
      for (let attempt = 0; attempt < 14; attempt++) {
        const a = hash2(i * 31 + attempt, camp.index) * Math.PI * 2;
        const r = 5 + hash2(attempt, i * 17 + camp.index) * (camp.id === 'ebc' ? 42 : 20);
        const x = camp.x + Math.cos(a) * r, z = camp.z + Math.sin(a) * r;
        if (camp.tents.some((t) => Math.hypot(t.x - x, t.z - z) < 5.5)) continue;
        this.data.normalAt(x, z, normal, 2.0);
        if (!best || normal[1] > best.flat) best = { x, z, flat: normal[1], n: [...normal] };
      }
      if (!best) continue;
      const big = camp.id === 'ebc' && i === 0;
      const material = new THREE.MeshStandardMaterial({
        color: TENT_COLORS[(i + camp.index) % TENT_COLORS.length], roughness: 0.85, metalness: 0, flatShading: false,
      });
      const tent = new THREE.Mesh(dome, material);
      tent.scale.set(big ? 3.4 : 1.35, big ? 2.1 : 1.05, big ? 4.6 : 1.9);
      tent.position.set(best.x, this.ground(best.x, best.z, -0.08), best.z);
      // Sit on the slope and face a consistent way so the camp reads as planned.
      const up = new THREE.Vector3(best.n[0], best.n[1], best.n[2]);
      tent.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), up);
      tent.rotateY(hash2(i, camp.index * 7) * 0.9 + 0.4);
      this.group.add(tent);
      camp.tents.push({ x: best.x, z: best.z });
    }
    // The camp's "door": the tent the player uses.
    camp.door = camp.tents[camp.id === 'ebc' ? 1 : 0] ?? { x: camp.x, z: camp.z };

    if (camp.id === 'ebc') {
      // Puja altar with strings of prayer flags, the heart of every Everest base camp.
      const cx = camp.x + 6, cz = camp.z - 4;
      const cy = this.ground(cx, cz);
      const stone = new THREE.MeshStandardMaterial({ color: 0x6f675f, roughness: 1 });
      const altar = new THREE.Mesh(new THREE.CylinderGeometry(0.9, 1.3, 1.5, 8), stone);
      altar.position.set(cx, cy + 0.7, cz);
      const pole = new THREE.Mesh(new THREE.CylinderGeometry(0.035, 0.05, 5.5, 6), new THREE.MeshStandardMaterial({ color: 0x8a6a3a }));
      pole.position.set(cx, cy + 3.6, cz);
      this.group.add(altar, pole);
      for (let k = 0; k < 6; k++) {
        const a = (k / 6) * Math.PI * 2 + 0.3;
        const ex = cx + Math.cos(a) * 26, ez = cz + Math.sin(a) * 26;
        this.flagStrings.push({ a: [cx, cy + 6.2, cz], b: [ex, this.ground(ex, ez, 0.4), ez] });
      }
    }
  }

  buildSummit(camp) {
    const y = this.ground(camp.x, camp.z);
    const metal = new THREE.MeshStandardMaterial({ color: 0xb8bcc2, roughness: 0.4, metalness: 0.8 });
    // The survey tripod that has marked the top since 1975.
    for (let k = 0; k < 3; k++) {
      const a = (k / 3) * Math.PI * 2;
      const leg = new THREE.Mesh(new THREE.CylinderGeometry(0.02, 0.02, 1.7, 5), metal);
      leg.position.set(camp.x + Math.cos(a) * 0.3, y + 0.75, camp.z + Math.sin(a) * 0.3);
      leg.rotation.set(Math.sin(a) * 0.35, 0, -Math.cos(a) * 0.35);
      this.group.add(leg);
    }
    for (let k = 0; k < 4; k++) {
      const a = (k / 4) * Math.PI * 2 + 0.6;
      const ex = camp.x + Math.cos(a) * 7, ez = camp.z + Math.sin(a) * 7;
      this.flagStrings.push({ a: [camp.x, y + 1.6, camp.z], b: [ex, this.ground(ex, ez, 0.15), ez] });
    }
    camp.door = { x: camp.x, z: camp.z };
    camp.tents = [];
  }

  buildFlagMesh() {
    const positions = [], colors = [], flutter = [], indices = [];
    const color = new THREE.Color();
    for (const string of this.flagStrings) {
      const [ax, ay, az] = string.a, [bx, by, bz] = string.b;
      const length = Math.hypot(bx - ax, by - ay, bz - az);
      const flags = Math.floor(length / 0.42);
      const dx = (bx - ax) / length, dz = (bz - az) / length;
      for (let i = 1; i < flags; i++) {
        const t = i / flags;
        const sag = Math.sin(t * Math.PI) * length * 0.07;
        const x = ax + (bx - ax) * t, y = ay + (by - ay) * t - sag, z = az + (bz - az) * t;
        const w = 0.15;
        const base = positions.length / 3;
        positions.push(x - dx * w, y, z - dz * w, x + dx * w, y, z + dz * w, x + dx * w, y - 0.3, z + dz * w, x - dx * w, y - 0.3, z - dz * w);
        color.setHex(FLAG_COLORS[i % 5]).convertSRGBToLinear();
        for (let v = 0; v < 4; v++) colors.push(color.r, color.g, color.b);
        flutter.push(0, 0, 1, 1);
        indices.push(base, base + 1, base + 2, base, base + 2, base + 3);
      }
    }
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
    geometry.setAttribute('color', new THREE.Float32BufferAttribute(colors, 3));
    geometry.setAttribute('flutter', new THREE.Float32BufferAttribute(flutter, 1));
    geometry.setIndex(indices);
    const mesh = new THREE.Mesh(geometry, this.flagMaterial);
    mesh.frustumCulled = false;
    return mesh;
  }

  // Bamboo wands with a scrap of red flag: how routes are marked across glaciers.
  buildWands() {
    const route = this.route;
    const spots = [];
    for (let s = 30; s < route.length - 20; s += 45) spots.push(route.pointAt(s, {}));
    const stick = new THREE.InstancedMesh(new THREE.CylinderGeometry(0.012, 0.012, 1.3, 5),
      new THREE.MeshStandardMaterial({ color: 0xa88a52, roughness: 1 }), spots.length);
    const flag = new THREE.InstancedMesh(new THREE.PlaneGeometry(0.22, 0.14),
      new THREE.MeshStandardMaterial({ color: 0xe02a1e, roughness: 1, side: THREE.DoubleSide, emissive: 0x400800 }), spots.length);
    const m = new THREE.Matrix4();
    spots.forEach((p, i) => {
      // Offset each wand a little to one side so the line is beside the track, not on it.
      const side = (hash2(i, 77) - 0.5) * 2.4;
      const x = p.x + side, z = p.z + side * 0.6;
      const y = this.ground(x, z);
      stick.setMatrixAt(i, m.makeTranslation(x, y + 0.62, z));
      flag.setMatrixAt(i, m.makeRotationY(hash2(i, 5) * 6.28).setPosition(x + 0.08, y + 1.2, z));
    });
    stick.frustumCulled = flag.frustumCulled = false;
    this.group.add(stick, flag);
  }

  // Fixed lines: a rope staked up the steep sections that climbers clip into.
  buildRopes() {
    const route = this.route;
    const material = new THREE.MeshStandardMaterial({ color: 0xf2a020, roughness: 0.8, emissive: 0x301800 });
    const stakeGeometry = new THREE.CylinderGeometry(0.02, 0.02, 0.9, 5);
    const stakeMaterial = new THREE.MeshStandardMaterial({ color: 0x9aa0a6, roughness: 0.5, metalness: 0.7 });
    let run = [];
    const flush = () => {
      if (run.length >= 2) {
        const curve = new THREE.CatmullRomCurve3(run, false, 'centripetal');
        const tube = new THREE.Mesh(new THREE.TubeGeometry(curve, run.length * 3, 0.022, 4, false), material);
        tube.frustumCulled = false;
        this.group.add(tube);
      }
      run = [];
    };
    for (let i = 0; i < route.n; i++) {
      if (!route.fixed[i]) { flush(); continue; }
      // Anchors every few points hold the rope off the snow; between them it sags to the surface.
      const anchor = i % 3 === 0;
      const x = route.x[i] + 1.1, z = route.z[i] + 0.4;
      const y = this.ground(x, z, anchor ? 0.75 : 0.22);
      run.push(new THREE.Vector3(x, y, z));
      if (anchor) {
        const stake = new THREE.Mesh(stakeGeometry, stakeMaterial);
        stake.position.set(x, y - 0.3, z);
        this.group.add(stake);
      }
    }
    flush();
  }

  // Aluminium ladders across the worst gaps of the Khumbu Icefall.
  buildLadders() {
    const route = this.route;
    const leg = route.legs.find((l) => l.hazards.includes('icefall'));
    if (!leg) return;
    const metal = new THREE.MeshStandardMaterial({ color: 0xc9ced4, roughness: 0.35, metalness: 0.85 });
    const s0 = route.s[leg.i0], s1 = route.s[leg.i1];
    for (let k = 1; k <= 7; k++) {
      const s = s0 + ((s1 - s0) * k) / 8;
      const a = route.pointAt(s - 2.5, {}), b = route.pointAt(s + 2.5, {});
      const ladder = new THREE.Group();
      const rail = new THREE.BoxGeometry(0.04, 0.06, 5.4);
      for (const side of [-0.2, 0.2]) {
        const r = new THREE.Mesh(rail, metal);
        r.position.x = side;
        ladder.add(r);
      }
      for (let j = -8; j <= 8; j++) {
        const rung = new THREE.Mesh(new THREE.BoxGeometry(0.4, 0.03, 0.04), metal);
        rung.position.z = j * 0.32;
        ladder.add(rung);
      }
      const ya = this.ground(a.x, a.z, 0.12), yb = this.ground(b.x, b.z, 0.12);
      ladder.position.set((a.x + b.x) / 2, (ya + yb) / 2, (a.z + b.z) / 2);
      ladder.lookAt(b.x, yb, b.z);
      this.group.add(ladder);
    }
  }

  update(sunVisibility) { this.flagMaterial.uniforms.uShade.value = sunVisibility; }
}
