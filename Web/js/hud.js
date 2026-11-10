// Heads-up display: altitude, weather, vitals, balance, navigation, map and messages.

const $ = (id) => document.getElementById(id);
const DEG = 180 / Math.PI;
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
const DANGER = ['', 'Low', 'Moderate', 'Considerable', 'High', 'Very high'];
const PX_PER_DEG = 60 / 45;

function setBar(id, fraction, color) {
  const el = $(id);
  el.style.width = `${clamp(fraction, 0, 1) * 100}%`;
  if (color) el.style.background = color;
}

const GREEN = 'var(--green)', AMBER = 'var(--amber)', RED = 'var(--red)', BLUE = 'var(--blue)';
const grade = (v, warnAt, badAt, lowIsBad = true) => {
  if (lowIsBad) return v <= badAt ? RED : v <= warnAt ? AMBER : GREEN;
  return v >= badAt ? RED : v >= warnAt ? AMBER : GREEN;
};

export class Hud {
  constructor(world, route) {
    this.world = world;
    this.route = route;
    this.el = $('hud');
    this.lastText = 0;
    this.toasts = [];
    this.mapImage = new Image();
    this.mapImage.src = `assets/${world.minimap}`;
    const names = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
    let html = '';
    for (let rep = 0; rep < 3; rep++) names.forEach((n, i) => { html += `<span class="${i % 2 ? 'minor' : ''}">${n}</span>`; });
    $('hud-compass-strip').innerHTML = html;
  }

  show(visible) { this.el.classList.toggle('hidden', !visible); }

  toast(text, kind = 'info') {
    const box = $('toasts');
    // Repeating the same line refreshes it instead of stacking copies.
    const existing = this.toasts.find((t) => t.text === text);
    if (existing) { existing.until = performance.now() + 5000; return; }
    const el = document.createElement('div');
    el.className = `toast ${kind}`;
    el.textContent = text;
    box.appendChild(el);
    const life = kind === 'danger' ? 9000 : kind === 'hint' ? 3500 : 6000;
    this.toasts.push({ el, text, until: performance.now() + life });
    while (this.toasts.length > 4) this.toasts.shift().el.remove();
  }

  prompt(html) {
    const el = $('prompt');
    if (!html) { el.classList.add('hidden'); return; }
    if (el.dataset.html !== html) { el.innerHTML = html; el.dataset.html = html; }
    el.classList.remove('hidden');
  }

  // Called every frame; text is refreshed a few times a second, moving parts every frame.
  update(game, camYaw, now) {
    const st = game.state;
    const sim = game.sim;
    const g = (k) => sim.get(k);

    // Messages fade out.
    for (let i = this.toasts.length - 1; i >= 0; i--) {
      const t = this.toasts[i];
      if (now > t.until) { t.el.remove(); this.toasts.splice(i, 1); } else if (now > t.until - 600) t.el.style.opacity = '0';
    }

    // Balance: the dot is where your weight is; keep it in the green.
    const balance = $('hud-balance');
    const active = !st.camp && !st.marching && st.move !== 'rest';
    const stability = g('B_STABILITY');
    balance.style.opacity = active ? (stability < 0.9 || Math.abs(st.lean) > 0.05 || g('B_SLIP') > 0.3 ? 1 : 0.45) : 0;
    const tilt = clamp(g('B_TILT_LAT') / 0.42, -1, 1);
    const a = tilt * 64 / DEG;
    const dot = $('bal-dot');
    dot.setAttribute('cx', (Math.sin(a) * 100).toFixed(1));
    dot.setAttribute('cy', (94 - Math.cos(a) * 100).toFixed(1));
    dot.style.fill = stability < 0.35 ? '#ef5a4a' : stability < 0.7 ? '#f2a51a' : '#ffffff';
    $('bal-lean').setAttribute('d', `M0 34 L${(st.lean * 26).toFixed(1)} 16`);
    $('bal-label').textContent = g('B_STUMBLING') > 0.5 ? 'STUMBLING' : game.bracing ? 'BRACING' : g('B_SLIP') > 0.6 ? 'SLIPPING' : 'BALANCE';

    // Compass.
    const heading = ((camYaw * DEG) % 360 + 360) % 360;
    $('hud-compass-strip').style.transform = `translateX(${-(heading + 360) * PX_PER_DEG - 30}px)`;

    this.drawMinimap(st, camYaw);
    if (now - this.lastText < 200) return;
    this.lastText = now;

    const alt = st.y;
    const kPa = g('W_PRESSURE');
    $('hud-alt').textContent = Math.round(alt).toLocaleString('en-US');
    $('hud-alt-sub').textContent = `${Math.round(alt * 3.28084).toLocaleString('en-US')} ft · ${kPa.toFixed(1)} kPa · ${Math.round(kPa / 101.325 * 100)}% of sea-level oxygen`;
    $('hud-clock').textContent = game.dateString();
    $('hud-warp').textContent = `×${game.warp}`;
    $('hud-march').classList.toggle('hidden', !st.marching);
    $('hud-rope').classList.toggle('hidden', !st.clipped);

    const temp = g('W_TEMP'), wind = g('W_WIND'), chill = g('W_WINDCHILL'), vis = g('W_VIS');
    $('hud-temp').textContent = `${temp.toFixed(0)}°C`;
    const windEl = $('hud-wind');
    windEl.textContent = `${wind.toFixed(0)} m/s`;
    windEl.className = wind > 20 ? 'bad' : wind > 13 ? 'warn' : '';
    // The arrow shows where the wind is going, relative to the way you are looking.
    $('hud-wind-arrow').style.transform = `rotate(${(g('W_DIR') + 180 - heading).toFixed(0)}deg)`;
    const chillEl = $('hud-chill');
    chillEl.textContent = `${chill.toFixed(0)}°C`;
    chillEl.className = chill < -40 ? 'bad' : chill < -28 ? 'warn' : '';
    $('hud-vis').textContent = vis >= 19000 ? 'clear' : vis >= 1000 ? `${(vis / 1000).toFixed(0)} km` : `${Math.max(10, Math.round(vis / 10) * 10)} m`;
    const danger = g('S_DANGER_LEVEL');
    const dangerEl = $('hud-danger');
    dangerEl.textContent = `${danger} ${DANGER[danger] ?? ''}`;
    dangerEl.className = danger >= 4 ? 'bad' : danger >= 3 ? 'warn' : '';
    $('hud-ground').textContent = st.camp ? 'in tent' : `${game._class?.name ?? ''} ${Math.round(game.baseSlope ?? 0)}°`;

    // Navigation: the next stop on the route.
    const s = game.routeS ?? 0;
    const upward = st.marching ? st.marching.upward : !st.summited;
    const target = st.marching ? this.route.camp(st.marching.target) : this.route.nextStop(s, upward);
    const dx = target.x - st.x, dz = target.z - st.z;
    const km = Math.abs(target.s - s) / 1000;
    const gain = target.y - alt;
    $('hud-target').textContent = `${target.short} · ${km < 1 ? `${Math.round(km * 1000)} m` : `${km.toFixed(1)} km`} · ${gain >= 0 ? '+' : '−'}${Math.abs(Math.round(gain)).toLocaleString('en-US')} m`
      + ((game.routeDist ?? 0) > 30 ? ` · route ${Math.round(game.routeDist)} m away` : '');
    let rel = Math.atan2(dx, -dz) * DEG - heading;
    rel = ((rel + 540) % 360) - 180;
    $('hud-compass-target').style.left = `${50 + clamp(rel * PX_PER_DEG / 4.2, -46, 46)}%`;
    $('hud-objective').textContent = game.objective();

    // Vitals.
    const spo2 = g('SPO2') * 100;
    const spo2El = $('v-spo2');
    spo2El.textContent = `${spo2.toFixed(0)}%`;
    spo2El.className = spo2 < 60 ? 'bad' : spo2 < 75 ? 'warn' : '';
    setBar('b-spo2', (spo2 - 40) / 60, grade(spo2, 75, 60));
    const effort = g('EXERTION') / Math.max(0.3, g('SUSTAINABLE'));
    $('v-effort').textContent = effort > 1.05 ? 'over limit' : `${Math.round(effort * 100)}%`;
    setBar('b-effort', effort / 1.4, grade(effort, 1.0, 1.2, false));
    const stamina = g('STAMINA');
    $('v-stamina').textContent = stamina < 0.15 ? 'gasping' : '';
    setBar('b-stamina', stamina, grade(stamina, 0.4, 0.15));
    const core = g('CORE');
    const coreEl = $('v-core');
    coreEl.textContent = `${core.toFixed(1)}°C`;
    coreEl.className = core < 35 || core > 38.6 ? 'bad' : core < 36 || core > 38.0 ? 'warn' : '';
    setBar('b-core', (core - 32) / 7, core < 35 ? RED : core < 36 ? BLUE : core > 38.0 ? AMBER : GREEN);
    for (const [id, key, fb] of [['v-hand', 'HAND_T', 'FBG_HAND'], ['v-foot', 'FOOT_T', 'FBG_FOOT'], ['v-face', 'FACE_T', 'FBG_FACE']]) {
      const el = $(id);
      const t = g(key);
      el.textContent = t.toFixed(0);
      el.className = g(fb) >= 1 || t < 5 ? 'bad' : t < 14 ? 'warn' : '';
    }
    const glycogen = g('GLYCOGEN');
    $('v-energy').textContent = `${Math.round(glycogen / 10) * 10} kcal`;
    setBar('b-energy', glycogen / 2000, grade(glycogen, 700, 300));
    const dehydration = g('DEHYDRATION');
    $('v-water').textContent = dehydration < 0.05 ? 'good' : `−${dehydration.toFixed(1)} L`;
    setBar('b-water', 1 - dehydration / 3.5, grade(-dehydration, -1.2, -2.2));
    const bottles = st.inv.o2;
    const o2El = $('v-o2');
    if (!bottles.length) { o2El.textContent = 'none carried'; setBar('b-o2', 0); } else {
      const hours = bottles[0] / st.gear.flow / 60;
      o2El.textContent = game.maskActive ? `${st.gear.flow} L/min · ${hours.toFixed(1)} h${bottles.length > 1 ? ` +${bottles.length - 1}` : ''}` : `off · ${bottles.length} bottle${bottles.length > 1 ? 's' : ''}`;
      setBar('b-o2', bottles[0] / game.bottleFullL, game.maskActive ? grade(hours, 2, 0.75) : 'rgba(255,255,255,0.4)');
    }
    $('v-health').textContent = st.injury > 0.02 ? `injured ${Math.round(st.injury * 100)}%` : '';
    setBar('b-health', 1 - st.injury, grade(1 - st.injury, 0.75, 0.45));
    $('v-pack').textContent = `${game.packMassKg().toFixed(1)} kg`;
    $('v-acclim').textContent = `${(Math.round(g('ACCLIM_ALT') * g('ACCLIM_VENT') / 100) * 100).toLocaleString('en-US')} m`;

    // Conditions.
    const conditions = [];
    const add = (text, mild) => conditions.push(`<div class="badge${mild ? ' mild' : ''}">${text}</div>`);
    if (g('HACE') > 0.1) add('HACE: cerebral edema');
    if (g('HAPE') > 0.1) add('HAPE: pulmonary edema');
    const ams = g('AMS');
    if (ams >= 3) add(`Altitude sickness (AMS ${ams.toFixed(0)})`, ams < 5);
    const hypo = g('HYPOTHERMIA');
    if (hypo >= 1) add(['', 'Mild hypothermia', 'Moderate hypothermia', 'Severe hypothermia'][hypo], hypo < 2);
    const fb = Math.max(g('FBG_HAND'), g('FBG_FOOT'), g('FBG_FACE'));
    if (fb >= 1) add(['', 'Frostnip', 'Frostbite', 'Deep frostbite'][fb], fb < 2);
    if (g('SNOWBLIND') > 0.25) add('Snow blindness', g('SNOWBLIND') < 0.6);
    if (st.wetness > 0.25) add('Clothing damp with sweat', true);
    if (g('DETERIORATION') > 0.3) add('Death-zone deterioration', true);
    if (game.avalancheRisk > 0.02 && !st.camp) add(`Avalanche slope: ${(game.avalancheRisk * 100).toFixed(0)}% trigger risk`, game.avalancheRisk < 0.06);
    const html = conditions.join('');
    const box = $('hud-conditions');
    if (box.dataset.html !== html) { box.innerHTML = html; box.dataset.html = html; }

    // The body changes what you see: tunnel vision when hypoxic, frost creeping in when cold.
    const cognitive = g('COGNITIVE');
    $('fx-vignette').style.opacity = clamp((75 - spo2) / 30 + cognitive * 0.5 + (stamina < 0.15 ? 0.25 : 0), 0, 0.88).toFixed(2);
    $('fx-frost').style.opacity = clamp((36 - core) / 4 + (10 - Math.min(g('HAND_T'), g('FACE_T'))) / 40, 0, 0.7).toFixed(2);
    const blur = clamp(cognitive * 5 + g('SNOWBLIND') * 6, 0, 7);
    document.getElementById('view').style.filter = blur > 0.4 ? `blur(${blur.toFixed(1)}px)` : '';
  }

  toMap(x, z, size) {
    const extent = this.world.near.extent_m;
    return [(x / extent + 0.5) * size, (z / extent + 0.5) * size];
  }

  drawRoute(ctx, scale, lineWidth) {
    const r = this.route;
    for (const fixed of [0, 1]) {
      ctx.beginPath();
      for (let i = 0; i < r.n - 1; i++) {
        if (r.fixed[i] !== fixed) continue;
        ctx.moveTo((r.x[i] / this.world.near.extent_m + 0.5) * scale, (r.z[i] / this.world.near.extent_m + 0.5) * scale);
        ctx.lineTo((r.x[i + 1] / this.world.near.extent_m + 0.5) * scale, (r.z[i + 1] / this.world.near.extent_m + 0.5) * scale);
      }
      ctx.strokeStyle = fixed ? '#ff5a3c' : '#ffd23c';
      ctx.lineWidth = lineWidth;
      ctx.stroke();
    }
  }

  // A 4 km window that turns with the camera, so "up" on the map is where you are looking.
  drawMinimap(st, camYaw) {
    if (!this.mapImage.complete || !this.mapImage.naturalWidth) return;
    const canvas = $('minimap');
    const ctx = canvas.getContext('2d');
    const size = canvas.width;
    const img = this.mapImage.naturalWidth;
    const span = 4000 / this.world.near.extent_m * img;
    const [px, pz] = this.toMap(st.x, st.z, img);
    ctx.save();
    ctx.fillStyle = '#1a2028';
    ctx.fillRect(0, 0, size, size);
    ctx.translate(size / 2, size / 2);
    ctx.rotate(-camYaw);
    const k = size / span;
    ctx.scale(k, k);
    ctx.translate(-px, -pz);
    ctx.drawImage(this.mapImage, 0, 0);
    this.drawRoute(ctx, img, 2.2 / k);
    for (const camp of this.route.stops) {
      const [cx, cz] = this.toMap(camp.x, camp.z, img);
      ctx.beginPath();
      ctx.arc(cx, cz, 4.5 / k, 0, Math.PI * 2);
      ctx.fillStyle = camp.id === 'summit' ? '#ffffff' : '#2f8cff';
      ctx.fill();
      ctx.lineWidth = 1.2 / k;
      ctx.strokeStyle = '#000';
      ctx.stroke();
    }
    ctx.restore();
    // Player marker: an arrow pointing the way the climber faces, relative to the view.
    ctx.save();
    ctx.translate(size / 2, size / 2);
    ctx.rotate(st.heading - camYaw);
    ctx.beginPath();
    ctx.moveTo(0, -9); ctx.lineTo(6, 7); ctx.lineTo(0, 3.5); ctx.lineTo(-6, 7); ctx.closePath();
    ctx.fillStyle = '#ff3b30';
    ctx.strokeStyle = '#fff';
    ctx.lineWidth = 1.5;
    ctx.fill();
    ctx.stroke();
    ctx.restore();
  }

  drawBigMap(st) {
    const canvas = $('bigmap-canvas');
    const ctx = canvas.getContext('2d');
    const size = canvas.width;
    if (this.mapImage.complete) ctx.drawImage(this.mapImage, 0, 0, size, size);
    this.drawRoute(ctx, size, 3);
    ctx.font = '600 14px system-ui, sans-serif';
    ctx.textBaseline = 'middle';
    const label = (text, x, y, color) => {
      ctx.lineWidth = 3.5;
      ctx.strokeStyle = 'rgba(0,0,0,0.75)';
      ctx.strokeText(text, x, y);
      ctx.fillStyle = color;
      ctx.fillText(text, x, y);
    };
    for (const mark of this.world.landmarks) {
      const [x, y] = this.toMap(mark.x, mark.z, size);
      label(`▲ ${mark.name} ${Math.round(mark.altitude_m).toLocaleString('en-US')}`, x + 6, y, '#e9d8ff');
    }
    for (const camp of this.route.stops) {
      const [x, y] = this.toMap(camp.x, camp.z, size);
      ctx.beginPath();
      ctx.arc(x, y, 6, 0, Math.PI * 2);
      ctx.fillStyle = camp.id === 'summit' ? '#fff' : '#2f8cff';
      ctx.fill();
      ctx.strokeStyle = '#000';
      ctx.lineWidth = 1.5;
      ctx.stroke();
      label(`${camp.short} ${Math.round(camp.y).toLocaleString('en-US')} m`, x + 10, y - 12, '#fff');
    }
    const [x, y] = this.toMap(st.x, st.z, size);
    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(st.heading);
    ctx.beginPath();
    ctx.moveTo(0, -12); ctx.lineTo(8, 9); ctx.lineTo(0, 4); ctx.lineTo(-8, 9); ctx.closePath();
    ctx.fillStyle = '#ff3b30';
    ctx.strokeStyle = '#fff';
    ctx.lineWidth = 2;
    ctx.fill();
    ctx.stroke();
    ctx.restore();
    // Scale bar.
    const km = size / (this.world.near.extent_m / 1000);
    ctx.fillStyle = '#fff';
    ctx.fillRect(20, size - 26, km * 2, 4);
    label('2 km', 20, size - 38, '#fff');
    label('N ↑', size - 50, 24, '#fff');
  }
}
