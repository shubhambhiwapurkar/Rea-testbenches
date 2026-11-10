// Entry point: loads the mountain and the simulation, then runs the frame loop that connects
// input, game rules, rendering, sound and the HUD.

import * as THREE from '../vendor/three.module.min.js';
import { Atmosphere, createEnvironmentUniforms } from './atmosphere.js';
import { GameAudio } from './audio.js';
import { ClimberFigure } from './figure.js';
import { Game } from './game.js';
import { Hud } from './hud.js';
import { WeatherParticles } from './particles.js';
import { Sim } from './sim.js';
import { Sky } from './sky.js';
import { TerrainData } from './terrainData.js';
import { TerrainView, buildNoiseTexture } from './terrainView.js';
import { Ui } from './ui.js';
import { Route, WorldObjects } from './world.js';

const params = new URLSearchParams(location.search);
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
const stored = (key) => { try { return localStorage.getItem(key); } catch (e) { return null; } };
const store = (key, value) => { try { localStorage.setItem(key, value); } catch (e) { /* private mode */ } };

function fatal(error) {
  const el = document.getElementById('fatal');
  el.textContent = `SUMMIT could not start.\n\n${error?.stack || error}\n\nThis game needs a browser with WebGL 2 and WebAssembly (any recent Chrome, Edge, Firefox or Safari).`;
  el.classList.remove('hidden');
  window.__error = String(error?.stack || error);
}

async function boot() {
  const lowQuality = params.get('q') === 'low' || (params.get('q') !== 'high' && stored('summit-low') === '1');
  const canvas = document.getElementById('view');
  const renderer = new THREE.WebGLRenderer({ canvas, antialias: !lowQuality, powerPreference: 'high-performance' });
  if (!renderer.capabilities.isWebGL2) throw new Error('WebGL 2 is not available.');
  renderer.setPixelRatio(lowQuality ? 1 : Math.min(window.devicePixelRatio || 1, 1.5));
  renderer.setSize(window.innerWidth, window.innerHeight, false);
  renderer.toneMapping = THREE.ACESFilmicToneMapping;

  const world = await (await fetch('assets/everest_world.json')).json();
  const dummyUi = { loading: (f, t) => { document.getElementById('load-bar').style.width = `${Math.round(f * 100)}%`; if (t) document.getElementById('load-text').textContent = t; } };
  dummyUi.loading(0.05, 'Loading 400 square kilometres of Himalaya…');
  const [data, sim] = await Promise.all([
    TerrainData.load('assets/', world, (f) => dummyUi.loading(0.05 + 0.75 * f)),
    Sim.load('assets/summit_sim.wasm'),
  ]);
  dummyUi.loading(0.85, 'Building the mountain…');
  await new Promise((r) => setTimeout(r, 30));

  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(58, window.innerWidth / window.innerHeight, 0.35, 220000);
  const env = createEnvironmentUniforms();
  const noise = buildNoiseTexture();
  // Real colour: a cloud-free Sentinel-2 image draped over the terrain.
  let satellite = null;
  if (world.satellite) {
    satellite = await new THREE.TextureLoader().loadAsync(`assets/${world.satellite}`).catch(() => null);
    if (satellite) {
      satellite.colorSpace = THREE.SRGBColorSpace;
      satellite.flipY = false;
      satellite.anisotropy = Math.min(8, renderer.capabilities.getMaxAnisotropy());
      satellite.needsUpdate = true;
    }
  }
  const terrain = new TerrainView(data, env, noise, lowQuality ? { quads: 64, finestCellDivisor: 4, detailShading: 0, satellite } : { satellite });
  scene.add(terrain.group);
  const sky = new Sky(env, noise, terrain.shared);
  scene.add(sky.group);
  const particles = new WeatherParticles(terrain.shared, { count: lowQuality ? 3500 : 9000 });
  scene.add(particles.group);
  const atmosphere = new Atmosphere(env, data.horizonDirections);

  const route = new Route(world, data);
  const objects = new WorldObjects(data, route, env);
  scene.add(objects.group);

  const figure = new ClimberFigure();
  scene.add(figure.root);
  const shadow = new THREE.Mesh(new THREE.CircleGeometry(0.55, 20).rotateX(-Math.PI / 2),
    new THREE.MeshBasicMaterial({ color: 0x000000, transparent: true, opacity: 0.35, depthWrite: false }));
  shadow.renderOrder = 20;
  scene.add(shadow);

  const sun = new THREE.DirectionalLight(0xffffff, 1);
  const hemisphere = new THREE.HemisphereLight(0xffffff, 0x444444, 1);
  const headlamp = new THREE.SpotLight(0xfff0d0, 0, 60, 0.5, 0.5, 1.4);
  scene.add(sun, sun.target, hemisphere, headlamp, headlamp.target);
  scene.fog = new THREE.FogExp2(0xffffff, 0.00001);

  const game = new Game(sim, data, route, world);
  const hud = new Hud(world, route);
  const audio = new GameAudio();
  let attract = true;        // the menu backdrop: a slow orbit with no HUD
  let shake = 0;
  let rumble = 0;
  let lastStepPhase = 0;

  const rig = { yaw: 2.0, pitch: 0.2, dist: 7.5, userAt: -1e9 };

  const ui = new Ui(game, {
    start: (mode) => startGame(mode),
    continueSave: () => {
      ui.hideAll();
      if (game.load()) { attract = false; ui.hideMenu(); hud.show(true); game.paused = false; if (game.state.camp) ui.openCamp(); } else game.toast('No saved climb found.', 'hint');
    },
    quit: () => { ui.hideAll(); game.paused = false; beginAttract(); ui.showMenu(Game.hasSave(), world.attribution); },
    hasSave: () => Game.hasSave(),
    setLow: (low) => { store('summit-low', low ? '1' : '0'); location.reload(); },
    setMuted: (muted) => audio.setMuted(muted),
  });
  document.getElementById('opt-low').checked = lowQuality;

  game.onToast = (text, kind) => hud.toast(text, kind);
  game.onEvent = (name) => {
    if (name === 'camp') ui.openCamp();
    else if (name === 'leave') ui.closeCamp();
    else if (name === 'campRefresh') ui.renderCamp();
    else if (name === 'summit') { audio.event('summit'); flash(); ui.showEnd('summit'); }
    else if (name === 'finished') ui.showEnd('finished');
    else if (name === 'dead') { if (!attract) setTimeout(() => ui.showEnd('dead'), 1800); }
    else if (name === 'fall') { audio.event('fall'); shake = 1; }
    else if (name === 'avalanche') { audio.event('fall'); shake = 2.5; rumble = 1; }
    else audio.event(name);
  };

  function flash() {
    const el = document.getElementById('fx-flash');
    el.style.transition = 'none';
    el.style.opacity = '0.8';
    requestAnimationFrame(() => { el.style.transition = ''; el.style.opacity = '0'; });
  }

  function beginAttract() {
    attract = true;
    hud.show(false);
    game.start('roam', Number(params.get('seed') ?? 7));
    game.teleport('c2');
    game.state.time = 149 * 86400 + 5.4 * 3600;
    document.getElementById('toasts').innerHTML = '';
    hud.toasts.length = 0;
    rig.yaw = 1.2; rig.pitch = 0.1; rig.dist = 260;
  }

  function startGame(mode) {
    audio.start();
    attract = false;
    ui.hideAll();
    ui.hideMenu();
    game.paused = false;
    document.getElementById('toasts').innerHTML = '';
    hud.toasts.length = 0;
    game.start(mode, Number(params.get('seed') ?? 7));
    hud.show(true);
    const st = game.state;
    rig.yaw = Math.atan2(route.summit.x - st.x, -(route.summit.z - st.z));
    rig.pitch = 0.16;
    rig.dist = 7.5;
    if (mode === 'push') {
      hud.toast('The storm is clearing. Enter the tent (F) to check the forecast and stock up, or press T to march up the route.', 'info');
    } else if (mode === 'expedition') {
      hud.toast('Climb high, sleep low. Go to Camp 1 and back before you sleep above Base Camp.', 'info');
    } else {
      hud.toast('Free roam: enter any tent (F) to jump between camps and change the time of day.', 'info');
    }
  }

  // ------------------------------------------------------------ input

  const keys = new Set();
  let dragging = false;
  const captureKeys = new Set(['Tab', 'Space', 'KeyW', 'KeyA', 'KeyS', 'KeyD', 'KeyQ', 'KeyE', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight']);

  window.addEventListener('keydown', (e) => {
    if (captureKeys.has(e.code)) e.preventDefault();
    if (e.repeat) return;
    keys.add(e.code);
    audio.start();
    if (attract) return;
    const st = game.state;
    if (e.code === 'Escape' || e.code === 'Tab') {
      if (ui.mapOpen) ui.toggleMap(hud);
      else if (st.camp && !ui.helpOpen) { if (e.code === 'Escape') game.leaveCamp(); }
      else if (!st.dead) { if (ui.toggleHelp() && document.pointerLockElement) document.exitPointerLock(); }
      return;
    }
    if (e.code === 'KeyM' && !st.camp && !ui.helpOpen) { ui.toggleMap(hud); if (document.pointerLockElement) document.exitPointerLock(); return; }
    if (game.paused || st.camp || st.dead) return;
    switch (e.code) {
      case 'Digit1': game.setWarp(0); break;
      case 'Digit2': game.setWarp(1); break;
      case 'Digit3': game.setWarp(2); break;
      case 'Digit4': game.setWarp(3); break;
      case 'KeyT': game.toggleMarch(true); break;
      case 'KeyB': game.toggleMarch(false); break;
      case 'KeyF': if (!game.enterCamp()) game.toggleClip(); break;
      case 'KeyR': game.toggleRest(); break;
      case 'KeyG': game.eatGel(); break;
      case 'KeyH': game.drink(); break;
      case 'KeyO': game.toggleMask(); break;
      case 'BracketLeft': game.changeFlow(-0.5); break;
      case 'BracketRight': game.changeFlow(0.5); break;
      case 'KeyC': game.toggleGear('crampons', 'Crampons on.', 'Crampons off.'); break;
      case 'KeyV': game.toggleGear('goggles', 'Goggles on.', 'Goggles off: the glare will burn your eyes within hours.'); break;
      case 'KeyP': game.toggleGear('poles', 'Trekking poles out: steadier on moderate ground.', 'Poles stowed.'); break;
      case 'KeyL': game.toggleGear('lamp', 'Headlamp on.', 'Headlamp off.'); st.lampManual = true; break;
      default: break;
    }
  });
  window.addEventListener('keyup', (e) => keys.delete(e.code));
  window.addEventListener('blur', () => keys.clear());

  canvas.addEventListener('mousedown', (e) => {
    audio.start();
    if (attract || game.paused) return;
    dragging = true;
    if (e.button === 0 && !document.pointerLockElement && params.get('nolock') === null) canvas.requestPointerLock?.()?.catch?.(() => {});
  });
  window.addEventListener('mouseup', () => { dragging = false; });
  window.addEventListener('mousemove', (e) => {
    if (attract || game.paused || game.state?.camp) return;
    if (document.pointerLockElement === canvas || dragging) {
      rig.yaw += e.movementX * 0.0026;
      rig.pitch = clamp(rig.pitch + e.movementY * 0.0022, -0.55, 1.35);
      rig.userAt = performance.now();
    }
  });
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    rig.dist = clamp(rig.dist * Math.exp(e.deltaY * 0.0012), 2.5, 600);
    rig.userAt = performance.now();
  }, { passive: false });
  window.addEventListener('resize', () => {
    renderer.setSize(window.innerWidth, window.innerHeight, false);
    camera.aspect = window.innerWidth / window.innerHeight;
    camera.updateProjectionMatrix();
  });

  function readInput() {
    const k = (code) => keys.has(code);
    const moveX = (k('KeyD') || k('ArrowRight') ? 1 : 0) - (k('KeyA') || k('ArrowLeft') ? 1 : 0);
    const moveY = (k('KeyW') || k('ArrowUp') ? 1 : 0) - (k('KeyS') || k('ArrowDown') ? 1 : 0);
    return {
      moveX, moveY, camYaw: rig.yaw,
      lean: (k('KeyE') ? 1 : 0) - (k('KeyQ') ? 1 : 0),
      brace: k('Space'),
      arrest: k('Space'),
      push: k('ShiftLeft') || k('ShiftRight'),
      careful: k('ControlLeft') || k('ControlRight') || k('AltLeft'),
    };
  }

  // ------------------------------------------------------------ frame loop

  const tmp = new THREE.Vector3();
  const look = new THREE.Vector3();
  const weather = {};
  let last = performance.now();
  let frames = 0;

  function frame(now) {
    requestAnimationFrame(frame);
    const dt = Math.min(0.1, (now - last) / 1000);
    last = now;
    const st = game.state;
    if (!st) return;

    const input = attract ? { moveX: 0, moveY: 0, camYaw: rig.yaw, lean: 0 } : readInput();
    if (attract) {
      // Menu backdrop: dawn breaking over the Western Cwm on a slow orbit.
      st.time += dt * 60;
      rig.yaw += dt * 0.02;
    }
    game.update(dt, input);
    if (st.camp && st.pending) ui.renderProgress();
    const g = (key) => sim.get(key);

    // Climber.
    figure.root.position.set(st.x, st.y, st.z);
    figure.root.rotation.y = Math.PI - st.heading;
    figure.root.visible = !st.camp;
    figure.setSuit(st.gear.suit);
    const ve = g('VE');
    const lampOn = st.gear.lamp && !st.camp;
    figure.update(dt, {
      mode: st.move, speed: st.speed, slopeDeg: game.slopeAlong ?? 0,
      tiltLat: g('B_TILT_LAT'), tiltFwd: g('B_TILT_FWD'), stumbling: g('B_STUMBLING') > 0.5,
      exertion: g('EXERTION'), shiver: g('SHIVER'), breathsPerMin: clamp(10 + ve * 0.42, 10, 60),
      mask: !!game.maskActive, goggles: st.gear.goggles, lampOn, bottles: st.inv.o2.length, time: now / 1000,
    });
    if (!st.lampManual) st.gear.lamp = g('SUN_ELEV') < -3;
    const stepPhase = Math.floor(figure.phase / Math.PI);
    if (stepPhase !== lastStepPhase && st.speed > 0.05 && !attract) audio.step(game._class?.sound ?? 'snow', st.gear.crampons);
    lastStepPhase = stepPhase;

    // Camera: orbit the climber. While marching it pulls back and swings behind on its own.
    const recentlyMoved = now - rig.userAt < 4000;
    if (st.marching && !recentlyMoved) {
      let d = st.heading - rig.yaw;
      d = Math.atan2(Math.sin(d), Math.cos(d));
      rig.yaw += d * Math.min(1, dt * 0.8);
      rig.dist += (26 - rig.dist) * Math.min(1, dt * 0.6);
      rig.pitch += (0.34 - rig.pitch) * Math.min(1, dt * 0.6);
    }
    const cp = Math.cos(rig.pitch), sp = Math.sin(rig.pitch);
    look.set(Math.sin(rig.yaw) * cp, -sp, -Math.cos(rig.yaw) * cp);
    tmp.set(st.x, st.y + (st.move === 'rest' ? 0.8 : 1.35), st.z);
    camera.position.copy(tmp).addScaledVector(look, -rig.dist);
    const groundAtCamera = data.height(clamp(camera.position.x, -data.half, data.half), clamp(camera.position.z, -data.half, data.half));
    if (camera.position.y < groundAtCamera + 0.7) camera.position.y = groundAtCamera + 0.7;
    if (shake > 0.01) {
      camera.position.x += (Math.random() - 0.5) * shake * 0.25;
      camera.position.y += (Math.random() - 0.5) * shake * 0.25;
      shake *= Math.exp(-dt * 2.2);
    }
    camera.lookAt(tmp);
    rumble *= Math.exp(-dt * 0.35);

    // Atmosphere from the simulation's weather.
    weather.sunElevDeg = g('SUN_ELEV');
    weather.sunAzDeg = g('SUN_AZ');
    weather.pressureKPa = g('W_PRESSURE');
    weather.cloud = g('W_CLOUD');
    weather.storm = g('W_STORM');
    weather.visibilityM = g('W_VIS');
    weather.windMS = g('W_WIND');
    weather.windFromDeg = g('W_DIR');
    weather.snowfallCmH = g('W_SNOW');
    weather.dayOfYear = g('DAY_OF_YEAR');
    weather.hour = g('HOUR');
    atmosphere.update(weather, camera.position.y, dt);
    env.uTime.value += dt;
    env.uFreshSnow.value = clamp(g('S_NEW72') / 35, 0, 1);
    env.uPlayerPos.value.set(st.x, st.y, st.z);
    renderer.toneMappingExposure = atmosphere.exposure;

    const sunVisible = game.sunlit ?? 1;
    sun.color.copy(env.uSunColor.value);
    sun.intensity = sunVisible;
    sun.position.copy(tmp).addScaledVector(env.uSunDir.value, 100);
    sun.target.position.copy(tmp);
    hemisphere.color.copy(env.uAmbientSky.value).add(env.uMoonColor.value);
    hemisphere.groundColor.copy(env.uAmbientGround.value);
    hemisphere.intensity = 1.0;
    scene.fog.color.copy(env.uFogColor.value);
    scene.fog.density = env.uFogDensity.value + env.uCloudFog.value / 70;

    // Headlamp: lights the figure's surroundings through three.js and the terrain through uniforms.
    const hx = Math.sin(st.heading), hz = -Math.cos(st.heading);
    env.uLampOn.value = lampOn ? 1 : 0;
    env.uLampPos.value.set(st.x + hx * 0.2, st.y + 1.72, st.z + hz * 0.2);
    env.uLampDir.value.set(hx, -0.32, hz).normalize();
    headlamp.intensity = lampOn ? 60 : 0;
    headlamp.position.copy(env.uLampPos.value);
    headlamp.target.position.copy(env.uLampPos.value).addScaledVector(env.uLampDir.value, 10);

    shadow.position.set(st.x, st.y + 0.06, st.z);
    shadow.material.opacity = st.camp ? 0 : 0.12 + 0.3 * sunVisible * Math.min(1, atmosphere.sunIntensity / 2);

    sky.update(camera, weather.hour, weather.cloud);
    terrain.update(camera);
    const snowy = game._class?.snowy ?? 1;
    particles.update(dt, weather.snowfallCmH, weather.windMS, snowy, window.innerHeight, camera.fov);
    objects.update(sunVisible);

    audio.update(dt, {
      windMS: weather.windMS, gustMS: g('W_GUST'), sheltered: !!st.camp,
      breathsPerMin: clamp(10 + ve * 0.42, 10, 60), exertion: attract ? 0 : g('EXERTION'), mask: !!game.maskActive, rumble: rumble * 0.6,
    });

    if (!attract) {
      hud.update(game, rig.yaw, now);
      let prompt = '';
      const camp = game.nearbyCamp();
      if (st.dead) prompt = '';
      else if (st.move === 'slide') prompt = st.slide?.avalanche ? 'Caught in the avalanche…' : '<kbd>Space</kbd> Self-arrest!';
      else if (st.move === 'fallen') prompt = 'Getting up…';
      else if (camp) prompt = `<kbd>F</kbd> Enter the tent at ${camp.short}`;
      else if (game.ropeAvailable && !st.clipped) prompt = '<kbd>F</kbd> Clip in to the fixed line';
      else if (st.move === 'rest') prompt = '<kbd>R</kbd> Stand up';
      else if (!st.marching && (game.routeDist ?? 99) < 25 && st.speed < 0.05) {
        const next = route.nextStop(game.routeS ?? 0, !st.summited);
        prompt = `<kbd>T</kbd> March up the route · <kbd>B</kbd> march down · next: ${next.short}`;
      }
      hud.prompt(st.camp ? '' : prompt);
    }

    renderer.render(scene, camera);
    frames++;
    if (frames === 3) window.__done = true;
  }

  beginAttract();
  ui.loading(1, '');
  ui.showMenu(Game.hasSave(), world.attribution);
  window.__summit = { game, sim, rig, camera, renderer, hud, ui, data, route, env, atmosphere, startGame, keys };
  const auto = params.get('autostart');
  if (auto) startGame(auto);
  requestAnimationFrame(frame);
}

boot().catch(fatal);
