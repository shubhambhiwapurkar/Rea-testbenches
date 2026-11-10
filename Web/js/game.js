// Game rules: turns player input and the terrain under the climber into inputs for the simulation,
// and turns what the simulation says back into movement limits, falls, slides, hazards and endings.

import { ACTIVITY, CAUSE_OF_DEATH, TERRAIN, TERRAIN_NAMES } from './sim.js';

const DEG = 180 / Math.PI;
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

export const WARPS = [1, 4, 16, 60];

// Clothing systems. clo values: 1 clo = 0.155 m²K/W. Boot mass is per pair.
export const SUITS = [
  { name: 'Trekking layers', torso: 2.4, hands: 1.4, feet: 2.0, face: 0.3, windproof: 0.6, bootsKg: 1.9, bag: -10 },
  { name: 'Mountaineering shell', torso: 4.6, hands: 2.4, feet: 3.0, face: 0.6, windproof: 0.85, bootsKg: 2.4, bag: -20 },
  { name: 'Down suit (8000 m)', torso: 7.5, hands: 3.2, feet: 3.6, face: 0.85, windproof: 0.92, bootsKg: 2.8, bag: -30 },
];

export const MODES = {
  push: {
    title: 'Summit push',
    blurb: 'You have finished your rotations and wait at Camp 2 (6,400 m) as a storm clears. Read the forecast, pick your window, climb to the top of the world and get back down.',
    day: 148, hour: 7.0, camp: 'c2', acclim: 2, suit: 1, goal: 'c2',
  },
  expedition: {
    title: 'Full expedition',
    blurb: 'Arrive at Base Camp on 10 April after the trek in. Acclimatize with rotations up the mountain over the coming weeks, then choose your summit window. Nothing is skipped.',
    day: 100, hour: 8.0, camp: 'ebc', acclim: 1, suit: 0, goal: 'ebc',
  },
  roam: {
    title: 'Free roam',
    blurb: 'Explore the real Everest massif without dying. Jump between camps, change the time of day and the weather, and watch the mountain.',
    day: 148, hour: 8.5, camp: 'ebc', acclim: 2, suit: 1, goal: null,
  },
};

const BOTTLE_EMPTY_KG = 2.7;
const BOTTLE_GAS_KG = 1.5;
const BASE_KIT_KG = 4.2;   // axe, harness, ascender, headlamp, spare mitts, first aid
const GEL_KCAL = 110;
const MEAL_KCAL = 650;
const SAVE_KEY = 'summit-everest-save-v1';

function campStock(mode) {
  const full = { gels: 12, meals: 6, gas: 4, o2: 0 };
  if (mode === 'expedition') {
    return {
      ebc: { gels: 60, meals: 60, gas: 30, o2: 6 }, c1: { ...full }, c2: { gels: 30, meals: 20, gas: 10, o2: 4 },
      c3: { gels: 10, meals: 6, gas: 4, o2: 3 }, c4: { gels: 10, meals: 4, gas: 4, o2: 5 },
    };
  }
  return {
    ebc: { gels: 60, meals: 60, gas: 30, o2: 6 }, c1: { ...full }, c2: { gels: 24, meals: 12, gas: 8, o2: 4 },
    c3: { gels: 10, meals: 6, gas: 4, o2: 3 }, c4: { gels: 10, meals: 4, gas: 4, o2: 5 },
  };
}

export class Game {
  constructor(sim, data, route, world) {
    this.sim = sim;
    this.data = data;
    this.route = route;
    this.world = world;
    this.bottleFullL = sim.bottleLiters(4, 300);
    this.onToast = () => {};
    this.onEvent = () => {};
    this.state = null;
    this._n = [0, 1, 0];
    this._fall = [0, 0];
    this._pt = {};
  }

  // ---------------------------------------------------------------- setup

  start(modeId, seed = 7) {
    const mode = MODES[modeId];
    const camp = this.route.camp(mode.camp);
    this.state = {
      mode: modeId, seed,
      time: mode.day * 86400 + mode.hour * 3600,
      startTime: mode.day * 86400 + mode.hour * 3600,
      x: camp.door.x + 3, z: camp.door.z + 3, y: 0,
      heading: 0, speed: 0, lean: 0,
      move: 'walk',              // walk | climb | rest | fallen | slide
      warp: 0,
      marching: null,            // { target: campId, upward }
      clipped: false,
      camp: null,                // camp id while inside a tent
      pending: null,             // timed camp action
      injury: 0, wetness: 0,
      gear: { suit: mode.suit, crampons: true, goggles: true, poles: false, mask: false, flow: 2, lamp: false,
        aceta: false, dex: false, nife: false, sleepO2: false },
      inv: { gels: 6, meals: 1, waterL: 1.0, o2: modeId === 'expedition' ? [] : [this.bottleFullL], extraKg: 0 },
      stock: campStock(modeId),
      visited: { [mode.camp]: true },
      slide: null, fallenTimer: 0, arrestCooldown: 0,
      summited: false, summitTime: 0, finished: false, dead: null,
      highPoint: 0, distance: 0, falls: 0, o2Used: 0,
      lastHazardCheck: 0, movedSinceCheck: 0, warnedGlacier: 0, lastVitalsWarning: 0,
      roamWeather: null,
    };
    this.sim.init({
      SEED: seed, VO2MAX: 56, BODY_MASS: 75, SKILL_TECH: 0.6, SKILL_ICE: 0.6, SKILL_ROPE: 0.6, AMS_SUSCEPT: 1,
      ACCLIM_PRESET: mode.acclim, LATITUDE: this.world.latitude, TIME: this.state.time,
    });
    this.afterLoad();
    this.toast(`${mode.title}: ${this.route.camp(mode.camp).short}, ${this.dateString()}`, 'info');
  }

  afterLoad() {
    const st = this.state;
    st.y = this.data.height(st.x, st.z);
    this.syncCargo();
    this.writeInputs(ACTIVITY.Resting, 0, 0, 0, 0);
    this.sim.tick(0, 0);
  }

  // In free roam the body is not simulated (so nothing can kill you), but weather, daylight and
  // balance still run.
  tick(dtSim, dtReal) { this.sim.tick(this.roam ? 0 : dtSim, dtReal); }

  save() {
    if (!this.state || this.state.dead) return false;
    try {
      const payload = { v: 1, state: this.state, body: this.sim.exportState() };
      localStorage.setItem(SAVE_KEY, JSON.stringify(payload));
      return true;
    } catch (e) {
      return false;
    }
  }

  static hasSave() {
    try { return !!localStorage.getItem(SAVE_KEY); } catch (e) { return false; }
  }

  load() {
    let payload;
    try { payload = JSON.parse(localStorage.getItem(SAVE_KEY)); } catch (e) { return false; }
    if (!payload || payload.v !== 1) return false;
    this.state = payload.state;
    const st = this.state;
    st.pending = null;
    st.slide = null;
    this.sim.init({
      SEED: st.seed, VO2MAX: 56, BODY_MASS: 75, SKILL_TECH: 0.6, SKILL_ICE: 0.6, SKILL_ROPE: 0.6, AMS_SUSCEPT: 1,
      ACCLIM_PRESET: MODES[st.mode].acclim, LATITUDE: this.world.latitude, TIME: st.time,
    });
    this.sim.importState(payload.body);
    this.afterLoad();
    this.toast(`Loaded: ${this.dateString()}`, 'info');
    return true;
  }

  // ---------------------------------------------------------------- helpers

  toast(text, kind = 'info') { this.onToast(text, kind); }
  get roam() { return this.state.mode === 'roam'; }
  get suit() { return SUITS[this.state.gear.suit]; }
  get warp() { return WARPS[this.state.warp]; }

  dateString() {
    const t = this.state.time;
    const day = Math.floor(t / 86400);
    const months = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
    const lengths = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    let m = 0, d = day;
    while (d >= lengths[m]) { d -= lengths[m]; m = (m + 1) % 12; }
    const h = (t % 86400) / 3600;
    return `${d + 1} ${months[m]} ${String(Math.floor(h)).padStart(2, '0')}:${String(Math.floor((h % 1) * 60)).padStart(2, '0')}`;
  }

  packMassKg() {
    const inv = this.state.inv;
    const bottles = inv.o2.reduce((sum, liters) => sum + BOTTLE_EMPTY_KG + BOTTLE_GAS_KG * (liters / this.bottleFullL), 0);
    return BASE_KIT_KG + inv.waterL * 1.05 + inv.gels * 0.045 + inv.meals * 0.16 + bottles
      + (inv.o2.length ? 0.6 : 0) + inv.extraKg;
  }

  // Tell the balance model what is in the pack and where. Oxygen bottles ride on alternate sides,
  // so an odd number of them pulls the load off-centre.
  syncCargo() {
    const inv = this.state.inv;
    const cargo = [{ massKg: BASE_KIT_KG + inv.waterL + inv.gels * 0.045 + inv.meals * 0.16 + inv.extraKg, x: 0, y: -0.16, z: 0.02 }];
    inv.o2.forEach((liters, i) => {
      cargo.push({ massKg: BOTTLE_EMPTY_KG + BOTTLE_GAS_KG * (liters / this.bottleFullL), x: i % 2 ? -0.13 : 0.13, y: -0.24, z: 0.05 });
    });
    this.sim.setCargo(cargo);
  }

  // What the ground is here, in the simulation's terms.
  classify(x, z, nearRouteDist) {
    const m = this.data.materialAt(x, z);
    const fresh = this.sim.get('S_NEW24');
    const onTrack = nearRouteDist < 6;
    const out = this._class ?? (this._class = {});
    out.glacier = m.glacier;
    out.snowy = clamp(m.snow + m.glacier * 0.7 + fresh / 12, 0, 1);
    let type;
    if (m.rock > 0.55 && fresh < 10) type = TERRAIN.Rock;
    else if (m.soil > 0.5 && fresh < 6) type = onTrack ? TERRAIN.Trail : (m.soil > 0.8 ? TERRAIN.Scree : TERRAIN.Boulder);
    else if (m.glacier > 0.55 && fresh < 4) type = TERRAIN.GlacierIce;
    else if (fresh > 30) type = TERRAIN.DeepSnow;
    else if (fresh > 9) type = TERRAIN.SoftSnow;
    else type = TERRAIN.HardSnow;
    // Traffic packs the trail: on the wanded line fresh snow is already broken.
    if (onTrack && type === TERRAIN.DeepSnow) type = TERRAIN.SoftSnow;
    else if (onTrack && type === TERRAIN.SoftSnow) type = TERRAIN.HardSnow;
    out.type = type;
    out.hard = type === TERRAIN.HardSnow || type === TERRAIN.GlacierIce || type === TERRAIN.Verglas;
    out.sound = type >= TERRAIN.HardSnow && type <= TERRAIN.DeepSnow ? 'snow' : type === TERRAIN.GlacierIce ? 'ice' : 'rock';
    out.freshCm = fresh;
    out.name = TERRAIN_NAMES[type];
    return out;
  }

  writeInputs(activity, speed, gradePercent, slopeAlongDeg, slopeAcrossDeg, lateralWind = 0, roughness = 0.3) {
    const st = this.state;
    const suit = this.suit;
    const inTent = !!st.camp;
    const sleeping = st.pending?.sleeping === true;
    const hasO2 = st.inv.o2.length > 0 && st.inv.o2[0] > 0;
    const maskOn = hasO2 && (sleeping ? st.gear.sleepO2 : st.gear.mask);
    const cls = this._class;
    const sunElev = this.sim.get('SUN_ELEV');
    const sunlit = inTent ? 0 : this.data.sunVisibility(st.x, st.z, sunElev, this.sim.get('SUN_AZ'));
    this.sunlit = sunlit;
    this.sim.set({
      TIME: st.time,
      ALT: st.y,
      EXPOSURE: inTent ? 0.04 : this.data.windExposure(st.x, st.z),
      SUNLIT: sunlit,
      TERRAIN: cls ? cls.type : TERRAIN.HardSnow,
      SNOW_DEPTH: cls ? Math.min(0.6, cls.freshCm / 100) : 0,
      IN_TENT: inTent,
      IN_BAG: inTent && (sleeping || st.pending?.kind === 'rest'),
      BAG_COMFORT: suit.bag,
      ACTIVITY: activity,
      SPEED: speed,
      GRADE: gradePercent,
      PACK_KG: inTent ? 0 : this.packMassKg(),
      FOOTWEAR_KG: suit.bootsKg,
      CRAMPONS: st.gear.crampons && !inTent,
      GOGGLES: st.gear.goggles || inTent,
      TORSO_CLO: suit.torso, HANDS_CLO: suit.hands, FEET_CLO: suit.feet, FACE_COVER: suit.face + (maskOn ? 0.1 : 0),
      WINDPROOF: suit.windproof, WETNESS: st.wetness,
      O2_MASK: maskOn, O2_FLOW: sleeping ? 0.5 : st.gear.flow,
      MED_ACETA: st.gear.aceta, MED_DEX: st.gear.dex, MED_NIFE: st.gear.nife,
      SLOPE_ALONG: slopeAlongDeg, SLOPE_ACROSS: slopeAcrossDeg,
      POLES: st.gear.poles && activity === ACTIVITY.Walking,
      LATERAL_WIND: lateralWind,
      ROUGHNESS: roughness,
      LEAN: st.lean,
      BRACING: this.bracing === true,
    });
    this.maskActive = maskOn;
  }

  // ---------------------------------------------------------------- per-frame update

  // input: { moveX, moveY, camYaw, lean (-1..1), brace, push, careful }
  update(dtReal, input) {
    const st = this.state;
    if (!st || st.dead || this.paused) return;
    dtReal = Math.min(dtReal, 0.1);
    if (st.camp) { this.updateCamp(dtReal); return; }

    const near = this.route.nearest(st.x, st.z);
    const routeDist = near.dist, routeIndex = near.index, routeS = near.s;
    const cls = this.classify(st.x, st.z, routeDist);
    this.routeDist = routeDist;
    this.routeS = routeS;
    this.routeIndex = routeIndex;
    const sim = this.sim;

    if (st.move === 'slide') { this.updateSlide(dtReal, input, cls); return; }
    if (st.move === 'fallen') {
      st.fallenTimer -= dtReal;
      st.speed = 0;
      this.writeInputs(ACTIVITY.Resting, 0, 0, 0, 0);
      this.tick(dtReal, 0);
      st.time += dtReal;
      if (st.fallenTimer <= 0) {
        st.move = 'walk';
        sim.recoverBalance();
      }
      this.afterTick(dtReal, 0);
      return;
    }

    // Lean and brace (the balance controls).
    st.lean += (clamp(input.lean, -1, 1) - st.lean) * Math.min(1, dtReal * 7);
    this.bracing = !!input.brace && !st.marching;

    let warp = this.warp;
    let dirX = 0, dirZ = 0, wantsMove = false;
    let marchStep = null;

    if (st.marching) {
      marchStep = this.marchDirection(routeS);
      if (!marchStep) { this.stopMarch(); } else {
        dirX = marchStep.dx; dirZ = marchStep.dz; wantsMove = true;
        if (input.moveX || input.moveY) { this.stopMarch('You take over.'); wantsMove = false; }
      }
    }
    if (!st.marching) {
      const mx = input.moveX, my = input.moveY;
      if (mx || my) {
        const sy = Math.sin(input.camYaw), cy = Math.cos(input.camYaw);
        dirX = sy * my + cy * mx;
        dirZ = -cy * my + sy * mx;
        const l = Math.hypot(dirX, dirZ);
        dirX /= l; dirZ /= l;
        wantsMove = true;
      }
      if (st.move === 'rest' && wantsMove) st.move = 'walk';
    }
    if (st.move === 'rest') wantsMove = false;

    // Face the direction of travel.
    if (wantsMove) {
      const target = Math.atan2(dirX, -dirZ);
      let d = target - st.heading;
      d = Math.atan2(Math.sin(d), Math.cos(d));
      st.heading += d * Math.min(1, dtReal * (st.marching ? 4 : 9));
    }
    const hx = Math.sin(st.heading), hz = -Math.cos(st.heading);
    const rx = Math.cos(st.heading), rz = Math.sin(st.heading);
    const d = this.data;
    const mdx = wantsMove ? dirX : hx, mdz = wantsMove ? dirZ : hz;

    // Slopes: over a boot length for balance, over a few metres for effort.
    const footAlong = Math.atan((d.height(st.x + mdx * 0.8, st.z + mdz * 0.8) - d.height(st.x - mdx * 0.8, st.z - mdz * 0.8)) / 1.6) * DEG;
    const footAcross = Math.atan((d.height(st.x - rx * 0.6, st.z - rz * 0.6) - d.height(st.x + rx * 0.6, st.z + rz * 0.6)) / 1.2) * DEG;
    const grade = (d.height(st.x + mdx * 3, st.z + mdz * 3) - d.height(st.x - mdx * 3, st.z - mdz * 3)) / 6;
    const slopeAlong = Math.atan(grade) * DEG;
    this.slopeAlong = slopeAlong;
    this.slopeAcross = footAcross;
    this.baseSlope = d.baseSlopeDeg(st.x, st.z);

    // Fixed lines.
    const ropeHere = this.route.isFixed(routeIndex) && routeDist < 5;
    this.ropeAvailable = ropeHere;
    if (st.clipped && !ropeHere) { st.clipped = false; if (!st.marching) this.toast('End of the fixed line: unclipped.', 'info'); }
    if (st.marching && ropeHere && !st.clipped) { st.clipped = true; this.onEvent('clip'); }

    // Activity for this step.
    const maxWalk = sim.get('MAX_WALK_SLOPE') || 35;
    let activity = ACTIVITY.Resting;
    let moveMode = st.move === 'rest' ? 'rest' : 'walk';
    let blocked = false;
    if (wantsMove) {
      const steep = Math.abs(slopeAlong);
      if (steep > 74) blocked = true;
      else if (steep > maxWalk) { activity = ACTIVITY.Climbing; moveMode = 'climb'; }
      else if (st.clipped && grade > 0.2) activity = ACTIVITY.Jumaring;
      else if (cls.type === TERRAIN.DeepSnow) activity = ACTIVITY.BreakingTrail;
      else activity = ACTIVITY.Walking;
      if (blocked) { activity = ACTIVITY.Resting; wantsMove = false; this.hint('Too steep to climb here without a rope.'); }
    }

    // Speed: what the body can sustain, capped by what the footing allows.
    const maxSpeed = sim.get('MAX_SPEED');
    const safeSpeed = sim.get('B_SAFE_SPEED') || 1.4;
    let pace = input.push ? 1.2 : input.careful ? 0.5 : 0.86;
    if (st.marching) pace = 0.85;
    if (sim.get('STAMINA') < 0.06) pace = Math.min(pace, 0.5);
    let target = 0;
    if (wantsMove) {
      target = Math.min(maxSpeed * pace, activity === ACTIVITY.Climbing ? 0.5 : Math.max(0.25, safeSpeed) * (input.push ? 1.25 : 1));
      target *= 1 - 0.65 * st.injury;
      if (sim.get('B_STUMBLING') > 0.5) target *= 0.35;
      if (this.bracing) target = Math.min(target, 0.12);
      // A body that cannot sustain any speed can still inch along (and must, to get down).
      target = Math.max(target, 0.05);
    }
    st.speed += (target - st.speed) * Math.min(1, dtReal * 3.5);
    if (!wantsMove && st.speed < 0.05) st.speed = 0;
    st.move = st.speed > 0.02 ? moveMode : (st.move === 'rest' ? 'rest' : 'walk');
    if (st.speed === 0 && st.move !== 'rest') activity = ACTIVITY.Resting;

    // Move. Speed is along the ground; the map distance covered shrinks on steep ground.
    const dtSim = dtReal * warp;
    if (st.speed > 0) {
      const along = st.speed * dtSim;
      const horizontal = along * Math.cos(slopeAlong / DEG);
      if (st.marching && marchStep) {
        const p = this.route.pointAt(routeS + (marchStep.upward ? along : -along), this._pt);
        st.x = p.x; st.z = p.z;
      } else {
        let nx = st.x + mdx * horizontal, nz = st.z + mdz * horizontal;
        if (st.clipped) {
          // The lanyard keeps a clipped climber within a couple of metres of the rope.
          const n2 = this.route.nearest(nx, nz);
          if (n2.dist > 3.5) { const k = 3.5 / n2.dist; nx = n2.x + (nx - n2.x) * k; nz = n2.z + (nz - n2.z) * k; }
        }
        st.x = clamp(nx, -d.bound, d.bound);
        st.z = clamp(nz, -d.bound, d.bound);
      }
      st.distance += along;
      st.movedSinceCheck += along;
    }
    st.y = d.height(st.x, st.z);

    // Wind across the climber: the sim reports where it blows from.
    const windTo = (sim.get('W_DIR') + 180) / DEG;
    const wind = sim.get('W_WIND');
    const lateralWind = (Math.sin(windTo) * rx - Math.cos(windTo) * rz) * wind;
    const roughness = cls.type === TERRAIN.Boulder ? 0.9 : cls.type === TERRAIN.Scree ? 0.7 : cls.type === TERRAIN.Rock ? 0.6
      : cls.glacier > 0.5 ? 0.5 : 0.25;

    this.writeInputs(activity, st.speed, grade * 100, footAlong, footAcross, lateralWind, roughness);
    st.time += dtSim;
    this.sim.set({ TIME: st.time, ALT: st.y });
    // While marching in time-lapse the balance model is not stepped: there is no player input for it.
    this.tick(dtSim, st.marching ? 0 : dtReal);

    this.afterTick(dtSim, dtReal, cls);
    if (st.dead || st.move === 'slide' || st.move === 'fallen') return;

    // Lost balance?
    if (!st.marching && sim.get('B_FALLEN') > 0.5) this.fall(cls, 'You lose your footing.');
    if (st.marching) this.checkMarchInterrupts(routeS);
  }

  hint(text) {
    const now = performance.now();
    if (this._hintText === text && now - this._hintAt < 4000) return;
    this._hintText = text;
    this._hintAt = now;
    this.toast(text, 'hint');
  }

  // Everything that follows a sim tick: consumables, hazards, vitals, endings.
  afterTick(dtSim, dtReal, cls) {
    const st = this.state;
    const sim = this.sim;

    // Bottled oxygen runs down at the regulator's flow rate.
    if (this.maskActive && st.inv.o2.length) {
      const flow = st.pending?.sleeping ? 0.5 : st.gear.flow;
      const used = flow * dtSim / 60;
      st.inv.o2[0] -= used;
      st.o2Used += used;
      if (st.inv.o2[0] <= 0) {
        st.inv.o2.shift();
        this.syncCargo();
        if (st.inv.o2.length) this.toast(`Bottle empty: switched to the next one (${st.inv.o2.length} left).`, 'warn');
        else { st.gear.mask = false; this.toast('OXYGEN EMPTY. You are breathing ambient air.', 'danger'); this.onEvent('warning'); this.dropWarp(); }
      }
    }

    // Sweat soaks insulation when you overheat; it dries slowly in cold dry air.
    const core = sim.get('CORE');
    if (core > 37.75) st.wetness = Math.min(0.7, st.wetness + (core - 37.7) * 0.9 * dtSim / 3600);
    else st.wetness = Math.max(0, st.wetness - 0.22 * dtSim / 3600);

    if (st.y > st.highPoint) st.highPoint = st.y;

    if (!st.camp && cls) this.checkHazards(dtSim, cls);
    this.checkVitals();

    // Summit.
    if (!st.summited) {
      const s = this.route.summit;
      if (Math.hypot(st.x - s.x, st.z - s.z) < 14) {
        st.summited = true;
        st.summitTime = st.time;
        this.stopMarch();
        this.dropWarp();
        this.onEvent('summit');
      }
    } else if (!st.finished) {
      const goal = MODES[st.mode].goal;
      if (goal && st.camp === goal) { st.finished = true; this.onEvent('finished'); }
    }

    // Death.
    const cause = sim.get('DEAD');
    if (!this.roam) {
      if (cause > 0) this.die(CAUSE_OF_DEATH[cause] ?? 'the mountain');
      else if (sim.get('INCAPACITATED') > 0.5 && !st.camp) this.die('exhaustion and exposure: you collapsed and could not go on');
      else if (st.injury >= 1) this.die(st.lastInjuryCause ?? 'a fall');
    } else {
      st.injury = Math.min(st.injury, 0.5);
    }
  }

  die(cause) {
    const st = this.state;
    if (st.dead) return;
    st.dead = cause;
    st.marching = null;
    st.speed = 0;
    st.move = 'fallen';
    this.onEvent('dead');
  }

  dropWarp() { this.state.warp = 0; }

  checkVitals() {
    const st = this.state;
    const sim = this.sim;
    const now = st.time;
    if (now - st.lastVitalsWarning < 1500 || this.roam) return;
    let warning = null;
    if (sim.get('HACE') > 0.3) warning = 'Confusion and loss of coordination: this is cerebral edema (HACE). Descend NOW.';
    else if (sim.get('HAPE') > 0.3) warning = 'Breathless at rest with a wet cough: pulmonary edema (HAPE). Descend NOW.';
    else if (sim.get('CORE') < 35.0) warning = 'You are hypothermic. Get out of the wind, add layers, eat, keep moving down.';
    else if (sim.get('FBG_HAND') >= 2 || sim.get('FBG_FOOT') >= 2 || sim.get('FBG_FACE') >= 2) warning = 'Frostbite is setting in. Warm the affected part or descend.';
    else if (sim.get('HAND_T') < 6 || sim.get('FOOT_T') < 6) warning = 'Your fingers and toes are going numb. Frostbite is close.';
    else if (sim.get('AMS') >= 5) warning = 'Severe altitude sickness. Do not go higher; descend if it does not improve.';
    else if (sim.get('DEHYDRATION') > 2.5) warning = 'You are badly dehydrated. Drink.';
    else if (sim.get('GLYCOGEN') < 250) warning = 'You are running on empty. Eat something.';
    else if (st.gear.mask === false && sim.get('SPO2') < 0.5 && st.inv.o2.length) warning = 'Blood oxygen is very low. Put your oxygen mask on (O).';
    if (warning) {
      st.lastVitalsWarning = now;
      this.toast(warning, 'danger');
      this.onEvent('warning');
      if (st.marching) this.stopMarch();
      this.dropWarp();
    }
  }

  // ---------------------------------------------------------------- hazards

  checkHazards(dtSim, cls) {
    const st = this.state;
    const sim = this.sim;
    st.lastHazardCheck += dtSim;
    if (st.lastHazardCheck < 20 && st.movedSinceCheck < 12) return;
    const elapsed = st.lastHazardCheck, moved = st.movedSinceCheck;
    st.lastHazardCheck = 0;
    st.movedSinceCheck = 0;
    if (this.roam) return;

    // Hidden crevasses: the wanded route threads between them; unmarked glacier does not.
    if (cls.glacier > 0.55 && this.routeDist > 10 && !st.clipped) {
      if (st.time - st.warnedGlacier > 3600) {
        st.warnedGlacier = st.time;
        this.toast('Unmarked glacier: hidden crevasses. The wanded route is the safe line.', 'warn');
      }
      const perMetre = 0.0010 + 0.003 * Math.min(1, cls.freshCm / 20);
      if (sim.random() < perMetre * moved) {
        const hurt = 0.18 + 0.75 * sim.random() ** 2;
        st.injury += hurt;
        st.lastInjuryCause = 'a fall into a hidden crevasse';
        st.falls++;
        this.stopMarch();
        this.dropWarp();
        st.move = 'fallen';
        st.fallenTimer = 9;
        st.time += 1800;
        this.onEvent('fall');
        this.toast(`The snow bridge collapses and you drop into a crevasse. ${hurt > 0.5 ? 'You are badly hurt.' : 'You climb out, shaken.'}`, 'danger');
        return;
      }
    }

    // Avalanches: only snow slopes of roughly 27-55 degrees can release as slabs.
    const slope = this.baseSlope ?? 0;
    this.avalancheRisk = 0;
    if (cls.snowy > 0.4 && slope > 27 && slope < 56) {
      const fall = this.data.fallLine(st.x, st.z, this._fall);
      const aspect = ((Math.atan2(fall[0], -fall[1]) * DEG) + 360) % 360;
      const windTo = (sim.get('W_DIR') + 180) / DEG;
      const leeward = fall[0] * Math.sin(windTo) - fall[1] * Math.cos(windTo) > 0.3;
      const odds = sim.avalanche(slope, aspect, st.y, leeward, false);
      this.avalancheRisk = odds.humanTrigger;
      const p = odds.humanTrigger * (moved / 150) + odds.naturalPerHour * (elapsed / 3600);
      if (sim.random() < p) this.avalanche();
    }
  }

  avalanche() {
    const st = this.state;
    const danger = this.sim.get('S_DANGER_LEVEL');
    this.stopMarch();
    this.dropWarp();
    st.clipped = false;
    st.falls++;
    st.move = 'slide';
    st.slide = { speed: 9, drop: 0, length: 0, time: 0, slopeSum: 0, samples: 0, startY: st.y, avalanche: true,
      carry: 120 + 480 * this.sim.random() * (0.4 + danger / 5) };
    this.onEvent('avalanche');
    this.toast('AVALANCHE! The slope breaks above you.', 'danger');
  }

  fall(cls, text) {
    const st = this.state;
    const sim = this.sim;
    st.falls++;
    this.stopMarch();
    this.dropWarp();
    this.onEvent('fall');
    const slope = this.baseSlope ?? 0;
    const slideAngle = cls.type === TERRAIN.GlacierIce ? 18 : cls.type === TERRAIN.HardSnow ? 27 : cls.type === TERRAIN.SoftSnow ? 34
      : cls.type === TERRAIN.DeepSnow ? 40 : 38;
    if (st.clipped && slope > 12) {
      // The fixed line takes the fall, if its anchor holds.
      const ageDays = Math.max(1, st.time / 86400 - 110);
      const r = sim.ropedFall(75 + this.packMassKg(), 1.2 + 2.5 * sim.random(), 9, Math.max(slope, 30), ageDays, sim.get('W_TEMP'));
      if (r.anchorFailed) {
        st.clipped = false;
        this.toast(`${text} The anchor rips out (${r.peakForceKN.toFixed(1)} kN)!`, 'danger');
      } else {
        st.injury += r.injury * 0.6;
        st.lastInjuryCause = 'a fall on the fixed lines';
        st.move = 'fallen';
        st.fallenTimer = 3 + 3 * sim.get('FATIGUE');
        this.toast(`${text} The fixed line catches you (${r.peakForceKN.toFixed(1)} kN on the anchor).`, 'warn');
        return;
      }
    }
    if (slope > slideAngle) {
      st.move = 'slide';
      st.slide = { speed: 1.5, drop: 0, length: 0, time: 0, slopeSum: 0, samples: 0, startY: st.y, avalanche: false, carry: 0 };
      this.toast(`${text} You are sliding: hit SPACE to self-arrest!`, 'danger');
    } else {
      st.move = 'fallen';
      st.fallenTimer = 2 + 4 * sim.get('FATIGUE');
      if (sim.random() < 0.1) { st.injury += 0.03 + 0.05 * sim.random(); st.lastInjuryCause = 'a fall'; }
      this.toast(text, 'warn');
    }
  }

  // Sliding on snow or ice, in real time. Self-arrest odds come from the sim.
  updateSlide(dtReal, input, cls) {
    const st = this.state;
    const sim = this.sim;
    const sl = st.slide;
    const d = this.data;
    const slope = d.baseSlopeDeg(st.x, st.z);
    const fall = d.fallLine(st.x, st.z, this._fall);
    const theta = slope / DEG;
    const friction = cls.type === TERRAIN.GlacierIce ? 0.04 : cls.type === TERRAIN.HardSnow ? 0.12 : cls.type === TERRAIN.SoftSnow ? 0.34
      : cls.type === TERRAIN.DeepSnow ? 0.52 : 0.6;
    let accel = 9.81 * (Math.sin(theta) - friction * Math.cos(theta)) - 0.012 * sl.speed * sl.speed;
    if (sl.avalanche && sl.length < sl.carry) accel = Math.max(accel, (14 - sl.speed) * 2);
    sl.speed = Math.max(0, sl.speed + accel * dtReal);
    const along = sl.speed * dtReal;
    st.x = clamp(st.x + fall[0] * along * Math.cos(theta), -d.bound, d.bound);
    st.z = clamp(st.z + fall[1] * along * Math.cos(theta), -d.bound, d.bound);
    st.y = d.height(st.x, st.z);
    st.heading = Math.atan2(fall[0], -fall[1]);
    sl.length += along;
    sl.time += dtReal;
    sl.slopeSum += slope;
    sl.samples++;
    sl.drop = Math.max(0, sl.startY - st.y);
    st.speed = sl.speed;

    st.arrestCooldown -= dtReal;
    let stopped = false;
    if (!sl.avalanche && input.arrest && st.arrestCooldown <= 0) {
      st.arrestCooldown = 0.75;
      const chance = sim.selfArrestChance(slope, cls.hard, 0.6, Math.max(0.3, sl.time), sim.get('FATIGUE')) * (sl.speed < 4 ? 1.3 : 1);
      if (sim.random() < chance) {
        stopped = true;
        this.onEvent('arrest');
        this.toast(`Self-arrest! The pick bites and you stop after ${sl.length.toFixed(0)} m.`, 'good');
      } else {
        this.onEvent('zip');
        this.hint(`The pick skates (${Math.round(chance * 100)}% chance). Try again!`);
      }
    }
    const ended = stopped || (sl.speed < 0.35 && accel <= 0 && (!sl.avalanche || sl.length >= sl.carry));
    this.writeInputs(ACTIVITY.Resting, 0, 0, 0, 0);
    st.time += dtReal;
    sim.set({ TIME: st.time, ALT: st.y });
    this.tick(dtReal, 0);

    if (ended) {
      const meanSlope = sl.slopeSum / Math.max(1, sl.samples);
      let hurt;
      if (sl.avalanche) {
        hurt = 0.2 + 0.8 * sim.random() ** 1.4 * (0.55 + this.sim.get('S_DANGER_LEVEL') / 8);
        st.lastInjuryCause = 'an avalanche';
        st.time += 1500;
        this.toast(hurt < 0.6 ? `The slide stops ${sl.length.toFixed(0)} m lower. You dig yourself out.` : 'You are buried and badly hurt.', 'danger');
      } else {
        hurt = sim.fallInjury(sl.drop, meanSlope) * (0.3 + 0.7 * sim.random()) * (stopped ? 0.5 : 1);
        st.lastInjuryCause = `a ${sl.drop.toFixed(0)} m fall`;
        if (!stopped) this.toast(`You come to rest ${sl.drop.toFixed(0)} m lower.`, 'warn');
      }
      st.injury += hurt;
      st.slide = null;
      st.move = 'fallen';
      st.fallenTimer = 3 + 5 * Math.min(1, hurt * 2);
      st.speed = 0;
    }
    this.afterTick(dtReal, dtReal, null);
  }

  // ---------------------------------------------------------------- marching (time-lapse along the route)

  // Start or stop following the wanded route toward the next camp, up or down.
  toggleMarch(upward) {
    const st = this.state;
    if (st.marching) { this.stopMarch('You stop.'); return; }
    if (st.move !== 'walk' && st.move !== 'rest' && st.move !== 'climb') return;
    const near = this.route.nearest(st.x, st.z);
    if (near.dist > 25) { this.toast('Get onto the wanded route first (within 25 m of the wands).', 'hint'); return; }
    const target = this.route.nextStop(near.s, upward);
    if (Math.abs(target.s - near.s) < 16) { this.toast(`You are already at ${target.short}.`, 'hint'); return; }
    st.marching = { target: target.id, upward };
    st.move = 'walk';
    st.warp = WARPS.length - 1;
    const leg = this.route.legAt(Math.min(near.index, this.route.n - 2));
    this.toast(`Marching ${upward ? 'up' : 'down'} to ${target.short}: ${(Math.abs(target.s - near.s) / 1000).toFixed(1)} km. ${leg.hazards.length ? 'Hazards: ' + leg.hazards.join(', ').replace(/_/g, ' ') + '.' : ''}`, 'info');
  }

  stopMarch(text) {
    const st = this.state;
    if (!st.marching) return;
    st.marching = null;
    st.warp = 0;
    if (text) this.toast(text, 'info');
  }

  marchDirection(routeS) {
    const st = this.state;
    const target = this.route.camp(st.marching.target);
    const upward = st.marching.upward;
    const remaining = upward ? target.s - routeS : routeS - target.s;
    if (remaining < 4) {
      this.toast(`You reach ${target.short} (${Math.round(target.y).toLocaleString()} m).`, 'good');
      return null;
    }
    const ahead = this.route.pointAt(routeS + (upward ? 6 : -6), this._pt);
    let dx = ahead.x - st.x, dz = ahead.z - st.z;
    const l = Math.hypot(dx, dz) || 1;
    return { dx: dx / l, dz: dz / l, upward };
  }

  checkMarchInterrupts() {
    const sim = this.sim;
    if (sim.get('W_GUST') > 24) this.stopMarch('Gusts are knocking you about: take control and brace.');
    else if (sim.get('STAMINA') < 0.12) {
      // Rest steps: stand and breathe until there is something in the tank again.
      this.state.speed = 0;
    }
  }

  // ---------------------------------------------------------------- player actions

  setWarp(index) {
    const st = this.state;
    if (st.camp || st.move === 'slide') return;
    index = clamp(index, 0, WARPS.length - 1);
    if (!st.marching && index === WARPS.length - 1) {
      this.toast(`×${WARPS[index]} is for marching the route (T). Free walking tops out at ×${WARPS[index - 1]}.`, 'hint');
      index = WARPS.length - 2;
    }
    st.warp = index;
  }

  toggleRest() {
    const st = this.state;
    if (st.marching) this.stopMarch();
    if (st.move === 'rest') st.move = 'walk';
    else if (st.move === 'walk' || st.move === 'climb') { st.move = 'rest'; st.speed = 0; }
  }

  toggleClip() {
    const st = this.state;
    if (st.clipped) { st.clipped = false; this.onEvent('clip'); this.toast('Unclipped from the fixed line.', 'info'); return; }
    if (!this.ropeAvailable) { this.toast('No fixed line within reach.', 'hint'); return; }
    st.clipped = true;
    this.onEvent('clip');
    this.toast('Clipped in to the fixed line.', 'good');
  }

  eatGel() {
    const st = this.state;
    if (st.inv.gels <= 0) { this.toast('No energy gels left.', 'hint'); return; }
    if (this.sim.get('APPETITE_KCAL') < 40) { this.toast('You cannot stomach any more right now.', 'hint'); return; }
    st.inv.gels--;
    this.sim.eat(GEL_KCAL);
    this.syncCargo();
    this.toast(`Energy gel (+${GEL_KCAL} kcal). ${st.inv.gels} left.`, 'good');
  }

  drink() {
    const st = this.state;
    if (st.inv.waterL < 0.05) { this.toast('Your bottle is empty. Melt snow at a camp.', 'hint'); return; }
    const amount = Math.min(0.25, st.inv.waterL);
    st.inv.waterL -= amount;
    this.sim.drink(amount);
    this.syncCargo();
    this.toast(`Drank ${(amount * 1000).toFixed(0)} ml. ${st.inv.waterL.toFixed(2)} L left.`, 'good');
  }

  toggleMask() {
    const st = this.state;
    if (!st.inv.o2.length) { this.toast('You are not carrying any oxygen bottles.', 'hint'); return; }
    st.gear.mask = !st.gear.mask;
    this.toast(st.gear.mask ? `Oxygen on at ${st.gear.flow} L/min.` : 'Oxygen off.', 'info');
  }

  changeFlow(delta) {
    const st = this.state;
    st.gear.flow = clamp(st.gear.flow + delta, 0.5, 4);
    const hours = st.inv.o2.length ? (st.inv.o2[0] / st.gear.flow / 60).toFixed(1) : '0';
    this.toast(`Oxygen flow ${st.gear.flow} L/min (this bottle lasts ${hours} h).`, 'info');
  }

  toggleGear(key, onText, offText) {
    const g = this.state.gear;
    g[key] = !g[key];
    this.toast(g[key] ? onText : offText, 'info');
  }

  // ---------------------------------------------------------------- camps

  nearbyCamp() {
    const st = this.state;
    if (st.camp) return null;
    for (const camp of this.route.stops) {
      if (camp.id === 'summit') continue;
      if (Math.hypot(st.x - camp.door.x, st.z - camp.door.z) < 16) return camp;
    }
    return null;
  }

  enterCamp() {
    const st = this.state;
    const camp = this.nearbyCamp();
    if (!camp) return false;
    this.stopMarch();
    st.camp = camp.id;
    st.visited[camp.id] = true;
    st.move = 'walk';
    st.speed = 0;
    st.clipped = false;
    st.warp = 0;
    this.save();
    this.onEvent('camp');
    return true;
  }

  leaveCamp() {
    const st = this.state;
    if (!st.camp || st.pending) return;
    const camp = this.route.camp(st.camp);
    st.camp = null;
    st.x = camp.door.x + 2.5;
    st.z = camp.door.z + 2.5;
    st.y = this.data.height(st.x, st.z);
    this.syncCargo();
    this.onEvent('leave');
  }

  // Timed things you do in a tent. hours of game time; the sky time-lapses while they run.
  campAction(kind, hours = 1) {
    const st = this.state;
    if (!st.camp || st.pending) return;
    const stock = st.stock[st.camp];
    if (kind === 'water') {
      if (stock.gas < 0.1) { this.toast('No stove gas left at this camp.', 'hint'); return; }
      stock.gas = Math.max(0, stock.gas - 0.12);
      st.pending = { kind, remaining: 0.6 * 3600, total: 0.6 * 3600, sleeping: false, label: 'Melting snow' };
    } else if (kind === 'meal') {
      if (stock.meals < 1 && st.inv.meals < 1) { this.toast('No meals left here.', 'hint'); return; }
      if (stock.gas < 0.1) { this.toast('No stove gas left at this camp.', 'hint'); return; }
      if (stock.meals >= 1) stock.meals--; else st.inv.meals--;
      stock.gas = Math.max(0, stock.gas - 0.15);
      st.pending = { kind, remaining: 0.75 * 3600, total: 0.75 * 3600, sleeping: false, label: 'Cooking a meal' };
    } else if (kind === 'rest') {
      st.pending = { kind, remaining: hours * 3600, total: hours * 3600, sleeping: false, label: 'Resting' };
    } else if (kind === 'sleep') {
      st.pending = { kind, remaining: hours * 3600, total: hours * 3600, sleeping: true, label: 'Sleeping' };
    }
  }

  // Hours from now until the next time the clock reads `hour`.
  hoursUntil(hour) {
    const now = (this.state.time % 86400) / 3600;
    let h = hour - now;
    if (h <= 0.25) h += 24;
    return h;
  }

  updateCamp(dtReal) {
    const st = this.state;
    const sim = this.sim;
    const p = st.pending;
    // One hour of game time per real second while something is running; real time otherwise.
    let dtSim = dtReal;
    if (p) dtSim = Math.min(p.remaining, dtReal * 3600);
    this._class = this.classify(st.x, st.z, 0);
    this.writeInputs(p?.sleeping ? ACTIVITY.Sleeping : ACTIVITY.Resting, 0, 0, 0, 0);
    st.time += dtSim;
    sim.set({ TIME: st.time });
    this.tick(dtSim, 0);
    if (p) {
      p.remaining -= dtSim;
      if (p.remaining <= 0) {
        st.pending = null;
        if (p.kind === 'water') {
          sim.drink(0.8);
          st.inv.waterL = Math.min(2, st.inv.waterL + 1.0);
          this.toast('Drank 0.8 L and filled your bottle. At this altitude water boils at ' + sim.get('W_BOILING').toFixed(0) + ' °C.', 'good');
        } else if (p.kind === 'meal') {
          const before = sim.get('APPETITE_KCAL');
          sim.eat(MEAL_KCAL);
          sim.drink(0.4);
          this.toast(before < MEAL_KCAL * 0.7 ? 'You force down part of the meal: altitude has killed your appetite.' : 'A hot meal.', 'good');
        } else if (p.kind === 'sleep') {
          this.toast(`You wake at ${this.dateString()}. SpO₂ ${(sim.get('SPO2') * 100).toFixed(0)}%, AMS ${sim.get('AMS').toFixed(1)}.`, 'info');
        }
        this.save();
        this.onEvent('campRefresh');
      }
    }
    this.afterTick(dtSim, 0, null);
  }

  // Move supplies between the tent and the pack. delta > 0 takes from the camp.
  transfer(item, delta) {
    const st = this.state;
    if (!st.camp) return;
    const stock = st.stock[st.camp];
    if (item === 'gels') {
      const n = delta > 0 ? Math.min(delta, stock.gels) : -Math.min(-delta, st.inv.gels);
      stock.gels -= n; st.inv.gels += n;
    } else if (item === 'meals') {
      const n = delta > 0 ? Math.min(delta, stock.meals) : -Math.min(-delta, st.inv.meals);
      stock.meals -= n; st.inv.meals += n;
    } else if (item === 'o2') {
      if (delta > 0 && stock.o2 > 0) { stock.o2--; st.inv.o2.push(this.bottleFullL); }
      // Only a full bottle goes back on the camp's pile; a part-used one stays with you.
      else if (delta < 0 && st.inv.o2.length) {
        const last = st.inv.o2[st.inv.o2.length - 1];
        st.inv.o2.pop();
        if (last > this.bottleFullL * 0.95) stock.o2++;
      }
      if (!st.inv.o2.length) st.gear.mask = false;
    }
    this.syncCargo();
    this.onEvent('campRefresh');
  }

  setSuit(index) {
    const st = this.state;
    if (st.gear.suit === index) return;
    st.gear.suit = index;
    this.toast(`Changed into: ${SUITS[index].name}.`, 'info');
    this.onEvent('campRefresh');
  }

  // Free roam: jump to a camp or set the clock.
  teleport(campId) {
    const st = this.state;
    const camp = this.route.camp(campId);
    this.stopMarch();
    st.camp = null;
    st.x = camp.door.x + 3; st.z = camp.door.z + 3;
    st.y = this.data.height(st.x, st.z);
    st.move = 'walk'; st.slide = null; st.clipped = false;
    this.toast(`${camp.short}: ${Math.round(st.y).toLocaleString()} m.`, 'info');
  }

  // The sim's clock only runs forward, so going "back" to an earlier hour means the next day.
  setHour(hour) {
    const st = this.state;
    st.time += this.hoursUntil(hour) * 3600;
    this.toast(this.dateString(), 'info');
  }

  // Objective line for the HUD.
  objective() {
    const st = this.state;
    if (st.mode === 'roam') return 'Free roam: explore. Keys 1-3 change time speed.';
    if (st.finished) return 'Expedition complete. You made it back.';
    if (st.summited) return `Summit reached. Now get down alive to ${this.route.camp(MODES[st.mode].goal).short}.`;
    if (st.mode === 'push') return 'Reach the summit (8,849 m) and return to Camp 2.';
    return 'Acclimatize, then reach the summit and return to Base Camp.';
  }
}
