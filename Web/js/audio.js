// Procedural sound: wind, breathing, footsteps and a few events, all synthesised from noise so the
// game ships no audio files. Everything follows the simulation: wind speed, breathing rate, surface.

export class GameAudio {
  constructor() {
    this.ctx = null;
    this.muted = false;
    this.breathPhase = 0;
    this.gust = 0;
    this.gustTarget = 0;
    this.gustTimer = 0;
  }

  // Browsers only allow audio after a user gesture, so this is called from the first click or key.
  start() {
    if (this.ctx) {
      if (this.ctx.state === 'suspended') this.ctx.resume();
      return;
    }
    const Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) return;
    const ctx = (this.ctx = new Ctx());
    const length = ctx.sampleRate * 4;
    const buffer = ctx.createBuffer(1, length, ctx.sampleRate);
    const d = buffer.getChannelData(0);
    for (let i = 0; i < length; i++) d[i] = Math.random() * 2 - 1;
    this.noiseBuffer = buffer;

    this.master = ctx.createGain();
    this.master.gain.value = 0.9;
    this.master.connect(ctx.destination);

    const band = (type, frequency, q) => {
      const source = ctx.createBufferSource();
      source.buffer = buffer;
      source.loop = true;
      source.playbackRate.value = 0.6 + Math.random() * 0.8;
      const filter = ctx.createBiquadFilter();
      filter.type = type;
      filter.frequency.value = frequency;
      filter.Q.value = q;
      const gain = ctx.createGain();
      gain.gain.value = 0;
      source.connect(filter).connect(gain).connect(this.master);
      source.start();
      return { filter, gain };
    };
    this.windLow = band('lowpass', 220, 0.4);     // the body of the wind
    this.windMid = band('bandpass', 620, 0.9);    // rush past the hood
    this.windHigh = band('bandpass', 1900, 5.0);  // whistle over ridges and guy lines
    this.breath = band('bandpass', 1100, 1.1);
    this.rumble = band('lowpass', 70, 0.7);
  }

  setMuted(muted) {
    this.muted = muted;
    if (this.master) this.master.gain.value = muted ? 0 : 0.9;
  }

  update(dt, s) {
    if (!this.ctx) return;
    const now = this.ctx.currentTime;
    // Gusts: wander between lulls and peaks every few seconds.
    this.gustTimer -= dt;
    if (this.gustTimer <= 0) {
      this.gustTimer = 1.2 + Math.random() * 3.5;
      this.gustTarget = Math.random();
    }
    this.gust += (this.gustTarget - this.gust) * Math.min(1, dt * 0.9);
    const gustiness = Math.max(0, s.gustMS - s.windMS) / Math.max(s.windMS, 1);
    const wind = Math.max(0, s.windMS * (1 + gustiness * (this.gust - 0.4))) * (s.sheltered ? 0.3 : 1);
    const level = Math.min(1, wind / 30);
    const set = (param, value) => param.setTargetAtTime(value, now, 0.15);
    set(this.windLow.gain.gain, 0.03 + 0.55 * level * level);
    set(this.windMid.gain.gain, 0.02 + 0.5 * Math.pow(level, 1.6));
    set(this.windMid.filter.frequency, 380 + 900 * level);
    set(this.windHigh.gain.gain, 0.22 * Math.pow(Math.max(0, level - 0.3), 2));
    set(this.windHigh.filter.frequency, 1500 + 1700 * level + 300 * this.gust);

    // Breathing: louder and faster with effort; muffled and hissing through an oxygen mask.
    this.breathPhase += (s.breathsPerMin / 60) * dt * Math.PI * 2;
    const cycle = Math.sin(this.breathPhase);
    const envelope = Math.pow(Math.abs(cycle), 0.7) * (cycle > 0 ? 1 : 0.7);
    set(this.breath.gain.gain, envelope * (0.015 + 0.16 * Math.min(1, s.exertion)) * (s.mask ? 1.5 : 1));
    set(this.breath.filter.frequency, (cycle > 0 ? 1400 : 800) * (s.mask ? 0.55 : 1));
    set(this.rumble.gain.gain, s.rumble);
  }

  burst(frequency, duration, volume, type = 'bandpass', q = 1.2) {
    if (!this.ctx || this.muted) return;
    const ctx = this.ctx;
    const source = ctx.createBufferSource();
    source.buffer = this.noiseBuffer;
    const filter = ctx.createBiquadFilter();
    filter.type = type;
    filter.frequency.value = frequency;
    filter.Q.value = q;
    const gain = ctx.createGain();
    const now = ctx.currentTime;
    gain.gain.setValueAtTime(volume, now);
    gain.gain.exponentialRampToValueAtTime(0.0008, now + duration);
    source.connect(filter).connect(gain).connect(this.master);
    source.start(now, Math.random() * 3, duration + 0.05);
  }

  // A boot going down. Snow squeaks, ice clicks under crampons, scree rattles.
  step(surface, crampons) {
    if (surface === 'snow') this.burst(1700 + Math.random() * 500, 0.16, 0.11, 'bandpass', 0.8);
    else if (surface === 'ice') this.burst(crampons ? 4200 : 2600, 0.06, 0.1, 'highpass', 1.5);
    else this.burst(900 + Math.random() * 600, 0.1, 0.1, 'bandpass', 0.6);
  }

  tone(frequency, duration, volume) {
    if (!this.ctx || this.muted) return;
    const ctx = this.ctx;
    const osc = ctx.createOscillator();
    osc.frequency.value = frequency;
    const gain = ctx.createGain();
    const now = ctx.currentTime;
    gain.gain.setValueAtTime(0, now);
    gain.gain.linearRampToValueAtTime(volume, now + 0.01);
    gain.gain.exponentialRampToValueAtTime(0.0008, now + duration);
    osc.connect(gain).connect(this.master);
    osc.start(now);
    osc.stop(now + duration + 0.05);
  }

  event(name) {
    if (name === 'clip') { this.burst(3200, 0.05, 0.25, 'highpass'); this.tone(1900, 0.06, 0.05); }
    else if (name === 'fall') this.burst(240, 0.5, 0.5, 'lowpass');
    else if (name === 'arrest') this.burst(2400, 0.35, 0.3, 'bandpass', 0.5);
    else if (name === 'radio') { this.burst(2600, 0.12, 0.08, 'bandpass', 3); this.tone(880, 0.09, 0.04); }
    else if (name === 'warning') { this.tone(520, 0.14, 0.07); setTimeout(() => this.tone(390, 0.2, 0.07), 150); }
    else if (name === 'summit') [523, 659, 784, 1046].forEach((f, i) => setTimeout(() => this.tone(f, 0.9, 0.06), i * 180));
    else if (name === 'zip') this.burst(3000, 0.25, 0.12, 'bandpass', 2);
  }
}
