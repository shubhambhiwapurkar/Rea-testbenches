// Loads the simulation core (Source/SummitSim compiled to WebAssembly) and exposes its shared
// array of doubles by field name. The field order is generated from the C++ bridge.

import { IO, IO_COUNT } from './simLayout.js';

const AUX_SIZE = 2048;

export class Sim {
  // Loads the WebAssembly build; if this page is not allowed to compile WebAssembly (some hosts
  // forbid it), falls back to the same module translated to plain JavaScript, which is slower but
  // computes identical results.
  static async load(url, fallbackUrl) {
    try {
      const response = await fetch(url);
      if (!response.ok) throw new Error(`Could not load the simulation (${response.status})`);
      const module = await WebAssembly.compile(await response.arrayBuffer());
      // The core does no I/O, but the C library still imports a few file calls; give them stubs.
      const imports = {};
      for (const entry of WebAssembly.Module.imports(module)) {
        (imports[entry.module] ??= {})[entry.name] = () => 0;
      }
      const instance = await WebAssembly.instantiate(module, imports);
      instance.exports._initialize?.();
      return new Sim(instance.exports, 'wasm');
    } catch (error) {
      if (!fallbackUrl) throw error;
      console.warn('WebAssembly unavailable, using the JavaScript build of the simulation:', error);
      const exports = await import(fallbackUrl);
      exports._initialize?.();
      return new Sim(exports, 'js');
    }
  }

  constructor(exports, kind = 'wasm') {
    this.x = exports;
    this.kind = kind;
    if (exports.sim_io_count() !== IO_COUNT) throw new Error('simLayout.js is out of date; rebuild the bridge');
    this.buffer = null;
    this.refreshViews();
  }

  // The WebAssembly heap can move when it grows, which detaches typed-array views.
  refreshViews() {
    if (this.buffer === this.x.memory.buffer) return;
    this.buffer = this.x.memory.buffer;
    this.io = new Float64Array(this.buffer, this.x.sim_io(), IO_COUNT);
    this.aux = new Float64Array(this.buffer, this.x.sim_aux(), AUX_SIZE);
  }

  get(name) { return this.io[IO[name]]; }

  set(values) {
    this.refreshViews();
    for (const key in values) {
      const v = values[key];
      this.io[IO[key]] = typeof v === 'boolean' ? (v ? 1 : 0) : v;
    }
  }

  init(setup) {
    this.set(setup);
    this.x.sim_init();
    this.refreshViews();
  }

  tick(dtSim, dtReal) {
    this.x.sim_tick(dtSim, dtReal);
    this.refreshViews();
  }

  eat(kcal) { this.x.sim_eat(kcal); }
  drink(liters) { this.x.sim_drink(liters); }
  recoverBalance() { this.x.sim_recover(); }
  kill(cause) { this.x.sim_kill(cause); }
  random() { return this.x.sim_random(); }
  selfArrestChance(slopeDeg, hard, skill, reactionS, fatigue) {
    return this.x.sim_self_arrest(slopeDeg, hard ? 1 : 0, skill, reactionS, fatigue);
  }
  fallInjury(distanceM, slopeDeg) { return this.x.sim_fall_injury(distanceM, slopeDeg); }
  ropedFall(massKg, aboveAnchorM, ropeOutM, slopeDeg, anchorAgeDays, temperatureC) {
    const injury = this.x.sim_roped_fall(massKg, aboveAnchorM, ropeOutM, slopeDeg, anchorAgeDays, temperatureC);
    this.refreshViews();
    return { injury, peakForceKN: this.aux[0], anchorFailed: this.aux[1] > 0.5 };
  }
  bottleLiters(waterVolumeL, fillBar) { return this.x.sim_bottle_liters(waterVolumeL, fillBar); }
  pressureKPa(altitudeM, dayOfYear) { return this.x.sim_pressure(altitudeM, dayOfYear); }

  avalanche(angleDeg, aspectDeg, elevationM, leeward, convex) {
    this.x.sim_avalanche(angleDeg, aspectDeg, elevationM, leeward ? 1 : 0, convex ? 1 : 0);
    return { naturalPerHour: this.get('AV_NATURAL_PER_HOUR'), humanTrigger: this.get('AV_HUMAN_TRIGGER') };
  }

  // cargo: [{ massKg, x, y, z, strapped }]
  setCargo(cargo) {
    this.refreshViews();
    cargo.forEach((item, i) => {
      this.aux.set([item.massKg, item.x, item.y, item.z, item.strapped === false ? 0 : 1], i * 5);
    });
    this.x.sim_set_cargo(cargo.length);
  }

  forecast(hoursAhead, stepHours, altitudeM) {
    const n = this.x.sim_forecast(hoursAhead, stepHours, altitudeM);
    this.refreshViews();
    const out = [];
    for (let i = 0; i < n; i++) {
      const a = this.aux.subarray(i * 8, i * 8 + 8);
      out.push({ time: a[0], tempC: a[1], windMS: a[2], gustMS: a[3], snowCmH: a[4], cloud: a[5], visibilityM: a[6], confidence: a[7] });
    }
    return out;
  }

  exportState() {
    const n = this.x.sim_export_state();
    this.refreshViews();
    return Array.from(this.aux.subarray(0, n));
  }

  importState(values) {
    this.refreshViews();
    this.aux.set(values);
    this.x.sim_import_state();
  }
}

export const CAUSE_OF_DEATH = ['', 'hypothermia', 'high-altitude pulmonary edema', 'high-altitude cerebral edema',
  'exhaustion', 'a fall', 'an avalanche', 'the mountain'];

// Mirrors summit::sim::TerrainType and Activity.
export const TERRAIN = Object.freeze({
  Trail: 0, Grass: 1, Scree: 2, Boulder: 3, Rock: 4, WetRock: 5, Verglas: 6, HardSnow: 7, SoftSnow: 8, DeepSnow: 9, GlacierIce: 10,
});
export const TERRAIN_NAMES = ['trail', 'grass', 'scree', 'boulders', 'rock', 'wet rock', 'verglas', 'hard snow', 'soft snow',
  'deep snow', 'glacier ice'];
export const ACTIVITY = Object.freeze({
  Resting: 0, Sleeping: 1, Walking: 2, Climbing: 3, Jumaring: 4, Rappelling: 5, BreakingTrail: 6,
});
