// Sun, moon, sky and fog colours for the current time, altitude and weather. Everything the shaders
// need lives in one shared uniform block so terrain, sky, clouds and particles always agree.

import * as THREE from '../vendor/three.module.min.js';

const DEG = Math.PI / 180;
const smooth = (a, b, x) => {
  const t = Math.min(1, Math.max(0, (x - a) / (b - a)));
  return t * t * (3 - 2 * t);
};
const mix = (a, b, t) => a + (b - a) * t;

export function directionFromAzEl(azimuthDeg, elevationDeg, out = new THREE.Vector3()) {
  const az = azimuthDeg * DEG;
  const el = elevationDeg * DEG;
  // Azimuth is clockwise from north; north is -z, east is +x, up is +y.
  return out.set(Math.sin(az) * Math.cos(el), Math.sin(el), -Math.cos(az) * Math.cos(el));
}

export function createEnvironmentUniforms() {
  return {
    uTime: { value: 0 },
    uSunDir: { value: new THREE.Vector3(0, 1, 0) },
    uSunColor: { value: new THREE.Color(1, 1, 1) },
    uSunElevDeg: { value: 45 },
    uSunBinA: { value: 0 },
    uSunBinB: { value: 1 },
    uSunBinT: { value: 0 },
    uMoonDir: { value: new THREE.Vector3(0, 1, 0) },
    uMoonColor: { value: new THREE.Color(0, 0, 0) },
    uSkyZenith: { value: new THREE.Color(0.1, 0.3, 0.8) },
    uSkyHorizon: { value: new THREE.Color(0.6, 0.75, 0.95) },
    uSunsetColor: { value: new THREE.Color(1.0, 0.45, 0.15) },
    uTwilight: { value: 0 },
    uNight: { value: 0 },
    uAmbientSky: { value: new THREE.Color(0.3, 0.4, 0.6) },
    uAmbientGround: { value: new THREE.Color(0.2, 0.2, 0.2) },
    uFogColor: { value: new THREE.Color(0.7, 0.8, 0.9) },
    uFogDensity: { value: 1 / 60000 },
    uCloudFog: { value: 0 },
    uCloudCover: { value: 0.2 },
    uCloudBase: { value: 5600 },
    uCloudTop: { value: 6000 },
    uStorm: { value: 0 },
    uWind: { value: new THREE.Vector2(5, 0) },
    uWindSpeed: { value: 5 },
    uFreshSnow: { value: 0 },
    uSnowfall: { value: 0 },
    uLampPos: { value: new THREE.Vector3() },
    uLampDir: { value: new THREE.Vector3(0, 0, -1) },
    uLampOn: { value: 0 },
    uPlayerPos: { value: new THREE.Vector3() },
  };
}

// Relative optical air mass (Kasten and Young 1989).
function airMass(elevationDeg) {
  const e = Math.max(elevationDeg, -1.5);
  return 1 / (Math.sin(e * DEG) + 0.50572 * Math.pow(e + 6.07995, -1.6364));
}

const tmp = new THREE.Color();

export class Atmosphere {
  constructor(uniforms, horizonDirections) {
    this.u = uniforms;
    this.horizonDirections = horizonDirections;
    this.exposure = 1;
    this.dayAmount = 1;
    this.sunIntensity = 1;
  }

  // weather: { sunElevDeg, sunAzDeg, pressureKPa, cloud, storm, visibilityM, windMS, windFromDeg,
  //            snowfallCmH, dayOfYear, hour }
  update(weather, cameraAltitude, dtReal) {
    const u = this.u;
    const el = weather.sunElevDeg;
    const pr = Math.min(1, weather.pressureKPa / 101.325);
    directionFromAzEl(weather.sunAzDeg, el, u.uSunDir.value);
    u.uSunElevDeg.value = el;

    const binWidth = 360 / this.horizonDirections;
    const f = (((weather.sunAzDeg % 360) + 360) % 360) / binWidth;
    u.uSunBinA.value = Math.floor(f) % this.horizonDirections;
    u.uSunBinB.value = (u.uSunBinA.value + 1) % this.horizonDirections;
    u.uSunBinT.value = f - Math.floor(f);

    const day = smooth(-7, 5, el);
    const cloud = Math.min(1, Math.max(0, weather.cloud));
    const storm = Math.min(1, Math.max(0, weather.storm));
    const overcast = Math.min(1, Math.pow(cloud, 1.6) * 0.75 + storm * 0.6);
    this.dayAmount = day;

    // Direct sunlight: Rayleigh and aerosol extinction along the slant path. Thinner air at
    // altitude means less extinction, which is why high-mountain light is so hard and white.
    const m = airMass(el) * pr;
    const tau = [0.058, 0.135, 0.331];
    const above = smooth(-1.6, 0.4, el);
    const direct = 3.4 * above * (1 - 0.92 * overcast);
    u.uSunColor.value.setRGB(
      direct * Math.exp(-(tau[0] + 0.06) * m),
      direct * Math.exp(-(tau[1] + 0.06) * m),
      direct * Math.exp(-(tau[2] + 0.06) * m),
    );
    this.sunIntensity = direct;

    // Moon: follows the sun's path shifted by its phase; full moon rises at sunset.
    const phase = (((weather.dayOfYear + weather.hour / 24) / 29.53 + 0.62) % 1 + 1) % 1;
    const moonHourShift = phase * 24;
    const moonHour = ((weather.hour - moonHourShift) % 24 + 24) % 24;
    const hourAngle = (moonHour - 12) * 15 * DEG;
    const lat = 28 * DEG;
    const decl = -8 * DEG;
    const sinEl = Math.sin(lat) * Math.sin(decl) + Math.cos(lat) * Math.cos(decl) * Math.cos(hourAngle);
    const moonEl = Math.asin(sinEl) / DEG;
    const moonAz = Math.atan2(Math.sin(hourAngle), Math.cos(hourAngle) * Math.sin(lat) - Math.tan(decl) * Math.cos(lat)) / DEG + 180;
    directionFromAzEl(moonAz, moonEl, u.uMoonDir.value);
    const illuminated = (1 - Math.cos(2 * Math.PI * phase)) / 2;
    const moon = 0.11 * illuminated * smooth(-2, 8, moonEl) * (1 - day) * (1 - 0.85 * overcast);
    u.uMoonColor.value.setRGB(0.62 * moon, 0.74 * moon, 1.0 * moon);

    // Sky dome. The zenith darkens with altitude: there is less air above to scatter light.
    const thin = 0.38 + 0.62 * pr;
    const zenith = u.uSkyZenith.value.setRGB(0.105 * thin, 0.27 * thin, 0.74 * (0.55 + 0.45 * pr));
    const horizon = u.uSkyHorizon.value.setRGB(0.50, 0.68, 0.93);
    zenith.multiplyScalar(day);
    horizon.multiplyScalar(day);
    // Twilight: a band of deep blue overhead and warm colour toward the sun.
    const twilight = smooth(-12, -2, el) * (1 - smooth(1, 9, el));
    zenith.add(tmp.setRGB(0.012, 0.02, 0.07).multiplyScalar(twilight));
    horizon.add(tmp.setRGB(0.10, 0.09, 0.14).multiplyScalar(twilight));
    u.uTwilight.value = Math.max(twilight, 1 - smooth(2, 14, el)) * above * (1 - overcast) + twilight * 0.6 * (1 - overcast);
    u.uSunsetColor.value.setRGB(1.0, mix(0.32, 0.62, smooth(-4, 10, el)), mix(0.10, 0.34, smooth(-4, 10, el)));
    // Night: starlight and airglow, plus scattered moonlight.
    const night = 1 - smooth(-12, -3, el);
    u.uNight.value = night * (1 - overcast);
    zenith.add(tmp.setRGB(0.0016, 0.0026, 0.0065).multiplyScalar(night)).add(tmp.copy(u.uMoonColor.value).multiplyScalar(0.10));
    horizon.add(tmp.setRGB(0.004, 0.006, 0.012).multiplyScalar(night)).add(tmp.copy(u.uMoonColor.value).multiplyScalar(0.16));
    // Cloud turns the sky grey.
    const grey = 0.62 * day + 0.012 * night + 0.05 * twilight;
    zenith.lerp(tmp.setRGB(grey * 0.86, grey * 0.9, grey * 0.97), overcast);
    horizon.lerp(tmp.setRGB(grey, grey * 1.02, grey * 1.06), overcast);

    // Ambient light on the ground: sky above, snow-bounced sunlight from below.
    u.uAmbientSky.value.copy(zenith).multiplyScalar(0.9).add(tmp.copy(horizon).multiplyScalar(0.75));
    u.uAmbientGround.value.copy(u.uSunColor.value).multiplyScalar(0.10).add(tmp.copy(horizon).multiplyScalar(0.35))
      .add(tmp.copy(u.uMoonColor.value).multiplyScalar(0.25));

    // Fog and aerial perspective. 3.912 / visibility is Koschmieder's relation for 2 % contrast.
    // The sim reports 20 km as "unlimited"; in clean high-mountain air the real limit is Rayleigh
    // scattering, well over 100 km, so stretch the top of the range out to that.
    const reported = Math.max(15, weather.visibilityM);
    const clearT = smooth(4000, 20000, reported);
    const visibility = mix(reported, 150000 / Math.max(0.35, pr), clearT * clearT);
    u.uFogDensity.value = 3.912 / visibility;
    u.uFogColor.value.copy(horizon).lerp(zenith, 0.25);
    u.uStorm.value = storm;
    u.uCloudCover.value = cloud;
    u.uSnowfall.value = weather.snowfallCmH;

    // Wind blows FROM windFromDeg, so it travels toward the opposite bearing.
    const to = (weather.windFromDeg + 180) * DEG;
    u.uWind.value.set(Math.sin(to) * weather.windMS, -Math.cos(to) * weather.windMS);
    u.uWindSpeed.value = weather.windMS;

    // Eye adaptation: open up at night so moonlit snow stays readable.
    const brightness = day * (1 - 0.45 * overcast) + 0.03;
    const target = mix(5.5, 0.82, Math.pow(Math.min(1, brightness), 0.45));
    this.exposure += (target - this.exposure) * Math.min(1, dtReal * 1.5);

    // In-cloud fog when the camera is inside the valley cloud layer.
    const inLayer = smooth(u.uCloudBase.value - 120, u.uCloudBase.value + 40, cameraAltitude)
      * (1 - smooth(u.uCloudTop.value - 60, u.uCloudTop.value + 160, cameraAltitude));
    u.uCloudFog.value = inLayer * smooth(0.25, 0.7, cloud);
  }
}
