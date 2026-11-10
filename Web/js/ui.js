// Menus: the start screen, the tent, help, and the end screens.

import { MODES, SUITS } from './game.js';

const $ = (id) => document.getElementById(id);
const show = (id, visible) => $(id).classList.toggle('hidden', !visible);
const DAY_NAMES = ['today', 'tomorrow'];

function button(label, onClick, { cls = '', disabled = false, title = '' } = {}) {
  const b = document.createElement('button');
  b.textContent = label;
  if (cls) b.className = cls;
  if (title) b.title = title;
  b.disabled = disabled;
  b.addEventListener('click', onClick);
  return b;
}

export class Ui {
  constructor(game, hooks) {
    this.game = game;
    this.hooks = hooks;   // { start(mode), continueSave(), quit(), setLow(bool), setMuted(bool) }
    $('help-close').addEventListener('click', () => this.toggleHelp(false));
    $('help-menu').addEventListener('click', () => hooks.quit());
    $('camp-leave').addEventListener('click', () => { game.leaveCamp(); });
    $('camp-save').addEventListener('click', () => { game.toast(game.save() ? 'Saved.' : 'Could not save in this browser.', 'info'); });
    $('opt-low').addEventListener('change', (e) => hooks.setLow(e.target.checked));
    $('opt-mute').addEventListener('change', (e) => hooks.setMuted(e.target.checked));
    $('btn-continue').addEventListener('click', () => hooks.continueSave());
  }

  loading(fraction, text) {
    $('load-bar').style.width = `${Math.round(fraction * 100)}%`;
    if (text) $('load-text').textContent = text;
  }

  showMenu(hasSave, attribution) {
    show('menu', true);
    show('loading', false);
    show('mode-list', true);
    show('menu-foot', true);
    show('btn-continue', hasSave);
    const list = $('mode-list');
    list.innerHTML = '';
    const tags = { push: 'ABOUT 30-60 MINUTES', expedition: 'THE LONG GAME', roam: 'NO DANGER' };
    for (const [id, mode] of Object.entries(MODES)) {
      const b = document.createElement('button');
      b.className = 'mode';
      b.innerHTML = `<strong>${mode.title}</strong><span>${mode.blurb}</span><em>${tags[id]}</em>`;
      b.addEventListener('click', () => this.hooks.start(id));
      list.appendChild(b);
    }
    $('credit').textContent = `Terrain ${attribution}. Body, weather, avalanche and rope models are calibrated against published field data; see the project docs.`;
  }

  hideMenu() { show('menu', false); }

  toggleHelp(force) {
    const open = force ?? $('help').classList.contains('hidden');
    show('help', open);
    this.game.paused = open;
    return open;
  }

  get helpOpen() { return !$('help').classList.contains('hidden'); }
  get mapOpen() { return !$('bigmap').classList.contains('hidden'); }

  toggleMap(hud) {
    const open = $('bigmap').classList.contains('hidden');
    show('bigmap', open);
    this.game.paused = open;
    if (open) hud.drawBigMap(this.game.state);
  }

  // ------------------------------------------------------------ camp

  openCamp() {
    show('camp', true);
    this.renderCamp();
  }

  closeCamp() { show('camp', false); }

  renderCamp() {
    const game = this.game;
    const st = game.state;
    if (!st.camp) return;
    const sim = game.sim;
    const g = (k) => sim.get(k);
    const camp = game.route.camp(st.camp);
    const stock = st.stock[st.camp];
    const busy = !!st.pending;
    $('camp-title').textContent = camp.name;
    $('camp-sub').textContent = `${Math.round(camp.y).toLocaleString('en-US')} m · ${game.dateString()} · outside ${g('W_TEMP').toFixed(0)}°C, wind ${g('W_WIND').toFixed(0)} m/s · water boils at ${g('W_BOILING').toFixed(0)}°C`;

    const actions = $('camp-actions');
    actions.innerHTML = '';
    const toDawn = game.hoursUntil(5);
    const toEvening = game.hoursUntil(21);
    actions.append(
      button('Melt snow & drink (35 min)', () => game.campAction('water'), { disabled: busy || stock.gas < 0.1 }),
      button('Hot meal (45 min)', () => game.campAction('meal'), { disabled: busy || (stock.meals < 1 && st.inv.meals < 1) || stock.gas < 0.1 }),
      button('Rest 1 h', () => game.campAction('rest', 1), { disabled: busy }),
      button('Rest 3 h', () => game.campAction('rest', 3), { disabled: busy }),
      button(`Sleep until 05:00 (${toDawn.toFixed(1)} h)`, () => game.campAction('sleep', toDawn), { disabled: busy }),
      button(`Sleep until 21:00 (${toEvening.toFixed(1)} h)`, () => game.campAction('sleep', toEvening), { disabled: busy, title: 'Summit pushes leave the South Col around 21:00-23:00' }),
      button('Wait a full day', () => game.campAction('rest', 24), { disabled: busy, title: 'Sit out bad weather' }),
      button(`Sleep on oxygen: ${st.gear.sleepO2 ? 'ON (0.5 L/min)' : 'off'}`, () => { st.gear.sleepO2 = !st.gear.sleepO2; this.renderCamp(); },
        { cls: st.gear.sleepO2 ? 'active' : '', disabled: busy || !st.inv.o2.length }),
    );

    const facts = [
      ['Blood oxygen', `${(g('SPO2') * 100).toFixed(0)}%`],
      ['Altitude sickness', `${g('AMS').toFixed(1)} / 12${g('AMS') >= 3 ? ' (AMS)' : ''}`],
      ['Acclimatized to', `${Math.round(g('ACCLIM_ALT') * g('ACCLIM_VENT')).toLocaleString('en-US')} m (${Math.round(g('ACCLIM_VENT') * 100)}% breathing, ${Math.round(g('ACCLIM_HEMA') * 100)}% blood)`],
      ['Core temperature', `${g('CORE').toFixed(1)}°C`],
      ['Energy stores', `${Math.round(g('GLYCOGEN'))} / 2000 kcal · appetite ${Math.round(g('APPETITE') * 100)}%`],
      ['Hydration', g('DEHYDRATION') < 0.05 ? 'good' : `${g('DEHYDRATION').toFixed(1)} L short`],
      ['Fatigue', `${Math.round(g('FATIGUE') * 100)}% · sleep debt ${g('SLEEP_DEBT').toFixed(1)} h`],
      ['Weight lost', `${g('MASS_LOST').toFixed(1)} kg`],
      ['Injuries', st.injury > 0.02 ? `${Math.round(st.injury * 100)}%` : 'none'],
    ];
    $('camp-body').innerHTML = facts.map(([k, v]) => `<span>${k}</span><span>${v}</span>`).join('');

    const supplies = $('camp-supplies');
    supplies.innerHTML = '';
    const row = (name, inTent, inPack, item, detail = '') => {
      const el = document.createElement('div');
      el.className = 'supply';
      el.innerHTML = `<span>${name}${detail ? ` <span class="muted">${detail}</span>` : ''}</span><span class="muted">tent ${inTent}</span><span>pack ${inPack}</span>`;
      el.append(button('Take', () => game.transfer(item, 1), { disabled: busy || inTent < 1 }), button('Leave', () => game.transfer(item, -1), { disabled: busy || inPack < 1 }));
      supplies.appendChild(el);
    };
    row('Energy gels', stock.gels, st.inv.gels, 'gels', '110 kcal');
    row('Meals', stock.meals, st.inv.meals, 'meals', '650 kcal');
    row('Oxygen bottles', stock.o2, st.inv.o2.length, 'o2', '4.2 kg, 9 h at 2 L/min');
    const info = document.createElement('div');
    info.className = 'muted';
    info.style.marginTop = '6px';
    info.textContent = `Stove gas: ${stock.gas.toFixed(1)} canisters · water carried: ${st.inv.waterL.toFixed(1)} L · pack ${game.packMassKg().toFixed(1)} kg`;
    supplies.appendChild(info);

    const gear = $('camp-gear');
    gear.innerHTML = '';
    SUITS.forEach((suit, i) => gear.appendChild(button(suit.name, () => game.setSuit(i), { cls: st.gear.suit === i ? 'active' : '', disabled: busy })));

    const meds = $('camp-meds');
    meds.innerHTML = '';
    const med = (key, label, title) => meds.appendChild(button(`${label}: ${st.gear[key] ? 'taking' : 'no'}`, () => { st.gear[key] = !st.gear[key]; this.renderCamp(); },
      { cls: st.gear[key] ? 'active' : '', title }));
    med('aceta', 'Acetazolamide', 'Speeds acclimatization and helps prevent altitude sickness');
    med('dex', 'Dexamethasone', 'Emergency treatment for severe altitude sickness and HACE; it masks symptoms, it does not acclimatize you');
    med('nife', 'Nifedipine', 'Treatment for HAPE');

    this.renderForecast();

    const roam = game.roam;
    show('camp-roam', roam);
    if (roam) {
      const box = $('camp-roam-buttons');
      box.innerHTML = '';
      for (const stop of game.route.stops) box.appendChild(button(`Go to ${stop.short}`, () => { game.teleport(stop.id); this.closeCamp(); }));
      for (const [label, hour] of [['Dawn', 5.2], ['Morning', 9], ['Noon', 12], ['Sunset', 18.4], ['Night', 23]]) {
        box.appendChild(button(label, () => { game.setHour(hour); this.renderCamp(); }));
      }
    }
    $('camp-leave').disabled = busy;
    this.renderProgress();
  }

  renderProgress() {
    const p = this.game.state.pending;
    show('camp-progress', !!p);
    if (!p) return;
    $('camp-bar').style.width = `${(1 - p.remaining / p.total) * 100}%`;
    $('camp-progress-text').textContent = `${p.label}… ${this.game.dateString()} (${(p.remaining / 3600).toFixed(1)} h left)`;
    $('camp-sub').textContent = `${this.game.dateString()} · outside ${this.game.sim.get('W_TEMP').toFixed(0)}°C, wind ${this.game.sim.get('W_WIND').toFixed(0)} m/s`;
  }

  // What the expedition's weather service would radio up: summit conditions every 12 hours.
  renderForecast() {
    const game = this.game;
    const entries = game.sim.forecast(120, 12, 8849);
    const today = Math.floor(game.state.time / 86400);
    let firstGo = null;
    const rows = entries.map((f) => {
      const day = Math.floor(f.time / 86400) - today;
      const hour = Math.round((f.time % 86400) / 3600);
      const go = f.windMS < 15 && f.snowCmH < 0.3;
      const marginal = !go && f.windMS < 20 && f.snowCmH < 0.6;
      if (go && firstGo === null && day >= 0) firstGo = { day, hour };
      const when = `${DAY_NAMES[day] ?? `in ${day} days`} ${String(hour).padStart(2, '0')}:00`;
      return `<tr class="${go ? 'go' : marginal ? '' : 'no'}"><td>${when}</td><td>${f.tempC.toFixed(0)}°C</td><td>${f.windMS.toFixed(0)} m/s</td><td>${f.gustMS.toFixed(0)}</td><td>${f.snowCmH.toFixed(1)}</td><td>${go ? 'GO' : marginal ? 'marginal' : 'NO'}</td><td>${Math.round(f.confidence * 100)}%</td></tr>`;
    });
    $('camp-forecast').innerHTML = `<table class="forecast"><tr><th>Summit, 8,849 m</th><th>Temp</th><th>Wind</th><th>Gust</th><th>Snow cm/h</th><th>Verdict</th><th>Confidence</th></tr>${rows.join('')}</table>`;
    $('camp-forecast-note').textContent = 'a climb needs wind under 15 m/s; forecasts get less reliable further out';
    const danger = game.sim.get('S_DANGER_LEVEL');
    let advice = firstGo ? `First summit window: ${DAY_NAMES[firstGo.day] ?? `in ${firstGo.day} days`} around ${String(firstGo.hour).padStart(2, '0')}:00.` : 'No summit window in the next five days.';
    if (danger >= 3) advice += ` Avalanche danger is ${['', 'low', 'moderate', 'considerable', 'high', 'very high'][danger]}: steep snow is dangerous.`;
    $('camp-advice').textContent = game.roam ? '' : advice;
  }

  // ------------------------------------------------------------ endings

  showEnd(kind) {
    const game = this.game;
    const st = game.state;
    const hours = (st.time - st.startTime) / 3600;
    const titles = { dead: 'YOU DID NOT COME BACK', summit: 'THE TOP OF THE WORLD', finished: 'HOME SAFE' };
    $('end-title').textContent = titles[kind];
    const usedO2 = st.o2Used > 5;
    let text = '';
    if (kind === 'dead') {
      text = `Cause: ${st.dead}. You reached ${Math.round(st.highPoint).toLocaleString('en-US')} m${st.summited ? ', and stood on the summit' : ''}. `
        + 'Everest has killed more than 330 climbers; most died on the descent, of the same things that happened here.';
    } else if (kind === 'summit') {
      text = `8,848.86 m, ${game.dateString()}. ${usedO2 ? 'You climbed on bottled oxygen, as about 97% of summiters do.' : 'Without bottled oxygen: fewer than 250 people have done that.'} `
        + 'The air here holds a third of the oxygen at sea level. You are only halfway: now get down.';
    } else {
      text = `You summited Everest and made it back to ${game.route.camp(MODES[st.mode].goal).short}. That is the whole job.`;
    }
    $('end-text').textContent = text;
    const stats = [
      ['Time on the mountain', hours < 48 ? `${hours.toFixed(1)} hours` : `${(hours / 24).toFixed(1)} days`],
      ['High point', `${Math.round(st.highPoint).toLocaleString('en-US')} m`],
      ['Distance on foot', `${(st.distance / 1000).toFixed(1)} km`],
      ['Bottled oxygen used', `${Math.round(st.o2Used).toLocaleString('en-US')} L`],
      ['Falls', `${st.falls}`],
      ['Frostbite', ['none', 'frostnip', 'superficial frostbite', 'deep frostbite: fingers or toes lost'][Math.max(game.sim.get('FBG_HAND'), game.sim.get('FBG_FOOT'), game.sim.get('FBG_FACE'))]],
      ['Weight lost', `${game.sim.get('MASS_LOST').toFixed(1)} kg`],
    ];
    $('end-stats').innerHTML = stats.map(([k, v]) => `<span>${k}</span><span>${v}</span>`).join('');
    const buttons = $('end-buttons');
    buttons.innerHTML = '';
    if (kind === 'dead') {
      if (this.hooks.hasSave()) buttons.appendChild(button('Reload last camp', () => { show('end', false); this.hooks.continueSave(); }, { cls: 'primary' }));
      buttons.appendChild(button('Back to menu', () => this.hooks.quit(), { cls: this.hooks.hasSave() ? 'ghost' : 'primary' }));
    } else {
      buttons.appendChild(button(kind === 'summit' ? 'Start the descent' : 'Keep exploring', () => { show('end', false); game.paused = false; }, { cls: 'primary' }));
      if (kind === 'finished') buttons.appendChild(button('Back to menu', () => this.hooks.quit(), { cls: 'ghost' }));
    }
    show('end', true);
    game.paused = kind !== 'dead';
  }

  hideAll() {
    for (const id of ['camp', 'help', 'bigmap', 'end']) show(id, false);
  }
}
