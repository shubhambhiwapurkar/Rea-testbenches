// The climber: a jointed figure built from simple shapes and animated procedurally from what the
// simulation says the body is doing (pace, slope, balance, breathing, shivering).

import * as THREE from '../vendor/three.module.min.js';

const SUITS = [
  { jacket: 0x2a6fb5, legs: 0x3a3f47, name: 'trekking layers' },
  { jacket: 0xe07a1f, legs: 0x2d3138, name: 'mountaineering shell' },
  { jacket: 0xd23a24, legs: 0xd23a24, name: 'down suit' },
];

function limb(radius, length, material) {
  const mesh = new THREE.Mesh(new THREE.CapsuleGeometry(radius, length, 3, 8), material);
  mesh.position.y = -length / 2 - radius * 0.5;
  return mesh;
}

export class ClimberFigure {
  constructor() {
    this.root = new THREE.Group();
    this.lean = new THREE.Group();     // pivots at the feet: balance tilt
    this.root.add(this.lean);
    this.hips = new THREE.Group();
    this.lean.add(this.hips);

    this.jacket = new THREE.MeshStandardMaterial({ color: SUITS[1].jacket, roughness: 0.9 });
    this.legMaterial = new THREE.MeshStandardMaterial({ color: SUITS[1].legs, roughness: 0.9 });
    const packMaterial = new THREE.MeshStandardMaterial({ color: 0x1f5a66, roughness: 0.95 });
    const bootMaterial = new THREE.MeshStandardMaterial({ color: 0xd9b21f, roughness: 0.8 });
    const dark = new THREE.MeshStandardMaterial({ color: 0x16181c, roughness: 0.6 });
    const skin = new THREE.MeshStandardMaterial({ color: 0xc98f6b, roughness: 0.8 });
    const metal = new THREE.MeshStandardMaterial({ color: 0xb9c0c8, roughness: 0.35, metalness: 0.8 });

    // Legs: hip joint -> thigh -> knee joint -> shin -> boot.
    this.legs = [-1, 1].map((side) => {
      const hip = new THREE.Group();
      hip.position.set(side * 0.105, 0, 0);
      hip.add(limb(0.075, 0.32, this.legMaterial));
      const knee = new THREE.Group();
      knee.position.y = -0.44;
      knee.add(limb(0.062, 0.32, this.legMaterial));
      const boot = new THREE.Mesh(new THREE.BoxGeometry(0.11, 0.1, 0.28), bootMaterial);
      boot.position.set(0, -0.44, 0.05);
      knee.add(boot);
      hip.add(knee);
      this.hips.add(hip);
      return { hip, knee, boot };
    });

    this.spine = new THREE.Group();
    this.hips.add(this.spine);
    this.torso = new THREE.Mesh(new THREE.CapsuleGeometry(0.17, 0.3, 4, 10), this.jacket);
    this.torso.position.y = 0.32;
    this.torso.scale.set(1.08, 1, 0.82);
    this.spine.add(this.torso);

    this.pack = new THREE.Mesh(new THREE.BoxGeometry(0.34, 0.5, 0.2), packMaterial);
    this.pack.position.set(0, 0.36, -0.2);
    this.spine.add(this.pack);
    this.bottle = new THREE.Mesh(new THREE.CylinderGeometry(0.055, 0.055, 0.42, 10), new THREE.MeshStandardMaterial({ color: 0xe8c21c, roughness: 0.4, metalness: 0.5 }));
    this.bottle.position.set(0.1, 0.36, -0.32);
    this.spine.add(this.bottle);

    this.head = new THREE.Group();
    this.head.position.y = 0.66;
    this.spine.add(this.head);
    const face = new THREE.Mesh(new THREE.SphereGeometry(0.105, 12, 10), skin);
    this.hood = new THREE.Mesh(new THREE.SphereGeometry(0.125, 12, 10, 0, Math.PI * 2, 0, Math.PI * 0.62), this.jacket);
    this.hood.rotation.x = -0.5;
    this.goggles = new THREE.Mesh(new THREE.BoxGeometry(0.17, 0.055, 0.06), dark);
    this.goggles.position.set(0, 0.02, 0.085);
    this.mask = new THREE.Mesh(new THREE.SphereGeometry(0.06, 8, 6), new THREE.MeshStandardMaterial({ color: 0x4a4f57, roughness: 0.7 }));
    this.mask.position.set(0, -0.045, 0.09);
    this.lamp = new THREE.Mesh(new THREE.BoxGeometry(0.05, 0.035, 0.03), new THREE.MeshBasicMaterial({ color: 0xfff2c8 }));
    this.lamp.position.set(0, 0.085, 0.1);
    this.head.add(face, this.hood, this.goggles, this.mask, this.lamp);

    // Arms: shoulder -> upper arm -> elbow -> forearm -> glove.
    this.arms = [-1, 1].map((side) => {
      const shoulder = new THREE.Group();
      shoulder.position.set(side * 0.23, 0.55, 0);
      shoulder.add(limb(0.055, 0.22, this.jacket));
      const elbow = new THREE.Group();
      elbow.position.y = -0.31;
      elbow.add(limb(0.048, 0.2, this.jacket));
      const glove = new THREE.Mesh(new THREE.SphereGeometry(0.058, 8, 6), dark);
      glove.position.y = -0.31;
      elbow.add(glove);
      shoulder.add(elbow);
      this.spine.add(shoulder);
      return { shoulder, elbow, glove };
    });

    // Ice axe in the right hand (the figure's right is -x when it faces +z).
    this.axe = new THREE.Group();
    const shaft = new THREE.Mesh(new THREE.CylinderGeometry(0.012, 0.012, 0.62, 6), metal);
    const pick = new THREE.Mesh(new THREE.BoxGeometry(0.26, 0.022, 0.03), metal);
    pick.position.y = 0.31;
    this.axe.add(shaft, pick);
    this.axe.position.y = -0.34;
    this.axe.rotation.x = Math.PI / 2 - 0.3;
    this.arms[0].elbow.add(this.axe);

    this.root.traverse((o) => { o.frustumCulled = false; });

    this.phase = 0;
    this.breath = 0;
    this.pose = { thigh: [0, 0], knee: [0, 0], arm: [0, 0], elbow: [0, 0], spine: 0, hipsY: 0.93, roll: 0, pitch: 0, sink: 0 };
    this.spin = 0;
    this.suit = -1;
    this.setSuit(1);
  }

  setSuit(index) {
    if (index === this.suit) return;
    this.suit = index;
    this.jacket.color.setHex(SUITS[index].jacket);
    this.legMaterial.color.setHex(SUITS[index].legs);
    const puff = index === 2 ? 1.18 : index === 1 ? 1.06 : 1.0;
    this.torso.scale.set(1.08 * puff, 1, 0.82 * puff);
  }

  // s: { mode, speed, slopeDeg, tiltLat, tiltFwd, stumbling, exertion, shiver, breathsPerMin, mask,
  //      goggles, lampOn, bottles, time }
  update(dt, s) {
    const p = this.pose;
    const moving = s.speed > 0.03 && (s.mode === 'walk' || s.mode === 'climb');
    // Cadence rises gently with pace; at altitude people take a breath or two per step.
    const cadence = moving ? Math.min(1.9, 0.75 + 0.9 * s.speed) : 0;
    this.phase += cadence * dt * Math.PI * 2;
    this.breath += (s.breathsPerMin / 60) * dt * Math.PI * 2;
    const up = Math.max(0, s.slopeDeg);
    const k = Math.min(1, dt * 9);
    const target = { thigh: [0, 0], knee: [0, 0], arm: [0, 0], elbow: [0.25, 0.25], spine: 0.05, hipsY: 0.93, roll: 0, pitch: 0, sink: 0 };

    if (s.mode === 'climb') {
      // Hands and tools on the slope: reach high, step high, body in to the wall.
      for (let i = 0; i < 2; i++) {
        const a = Math.sin(this.phase + i * Math.PI);
        target.thigh[i] = -0.75 - 0.45 * a;
        target.knee[i] = 1.0 + 0.5 * a;
        target.arm[i] = -2.2 + 0.45 * Math.sin(this.phase + i * Math.PI + 1.2);
        target.elbow[i] = 0.5;
      }
      target.spine = 0.25;
      target.pitch = Math.min(0.9, (up - 25) * 0.018);
      target.hipsY = 0.8;
    } else if (s.mode === 'rest') {
      for (let i = 0; i < 2; i++) {
        target.thigh[i] = -1.45;
        target.knee[i] = 1.55;
        target.arm[i] = -0.5;
        target.elbow[i] = 0.9;
      }
      target.hipsY = 0.36;
      target.spine = 0.3;
    } else if (s.mode === 'fallen' || s.mode === 'slide') {
      for (let i = 0; i < 2; i++) {
        target.thigh[i] = -0.3 + i * 0.4;
        target.knee[i] = 0.5;
        target.arm[i] = s.mode === 'slide' ? -2.4 : -1.0;
        target.elbow[i] = 0.4;
      }
      target.pitch = -1.45;
      target.hipsY = 0.25;
      target.sink = 0.05;
    } else {
      const stride = (0.28 + 0.34 * Math.min(1, s.speed / 1.1)) * (moving ? 1 : 0) * (1 - Math.min(0.5, up / 90));
      for (let i = 0; i < 2; i++) {
        const a = Math.sin(this.phase + i * Math.PI);
        const lift = Math.max(0, Math.cos(this.phase + i * Math.PI));
        target.thigh[i] = -stride * a - up * 0.006 * (moving ? 1 : 0) - lift * up * 0.012;
        target.knee[i] = (0.25 + up * 0.012) * lift * (moving ? 1 : 0) + 0.06;
        target.arm[i] = stride * 0.85 * a;
        target.elbow[i] = 0.3 + 0.15 * Math.abs(a);
      }
      target.spine = 0.05 + Math.min(0.5, up * 0.012) + s.exertion * 0.08;
      target.hipsY = 0.93 - 0.015 * Math.abs(Math.sin(this.phase * 2)) * (moving ? 1 : 0) - Math.min(0.06, up * 0.0015);
      if (s.stumbling) {
        target.arm[0] = -1.6;
        target.arm[1] = -1.9;
        target.spine += 0.25;
      }
    }

    const lerp = (a, b) => a + (b - a) * k;
    for (let i = 0; i < 2; i++) {
      p.thigh[i] = lerp(p.thigh[i], target.thigh[i]);
      p.knee[i] = lerp(p.knee[i], target.knee[i]);
      p.arm[i] = lerp(p.arm[i], target.arm[i]);
      p.elbow[i] = lerp(p.elbow[i], target.elbow[i]);
      this.legs[i].hip.rotation.x = p.thigh[i];
      this.legs[i].knee.rotation.x = p.knee[i];
      this.arms[i].shoulder.rotation.x = p.arm[i];
      this.arms[i].elbow.rotation.x = -p.elbow[i];
    }
    p.spine = lerp(p.spine, target.spine);
    p.hipsY = lerp(p.hipsY, target.hipsY);
    p.pitch = lerp(p.pitch, target.pitch);
    this.hips.position.y = p.hipsY;

    // Balance from the sim tilts the whole body about the feet. The figure's right is -x, so a
    // positive (rightward) lateral tilt is a positive rotation about its forward axis.
    const shake = s.shiver > 0.05 ? (Math.sin(s.time * 47) + Math.sin(s.time * 31.7)) * 0.012 * s.shiver : 0;
    this.lean.rotation.z = lerp(this.lean.rotation.z, s.tiltLat * 1.6) + shake;
    this.lean.rotation.x = lerp(this.lean.rotation.x, s.tiltFwd * 1.2 + p.pitch) + shake * 0.6;
    if (s.mode === 'slide') this.spin += dt * 2.2; else this.spin = lerp(this.spin, 0);
    this.lean.rotation.y = this.spin;

    this.spine.rotation.x = p.spine;
    const breathing = Math.sin(this.breath) * (0.012 + 0.03 * s.exertion);
    this.torso.scale.z = 0.82 * (this.suit === 2 ? 1.18 : 1.03) * (1 + breathing);
    this.spine.position.y = 0.04 + breathing * 0.15;
    this.head.rotation.x = -p.spine * 0.6 + (s.exertion > 0.85 ? Math.sin(this.breath) * 0.05 : 0);

    this.mask.visible = s.mask;
    this.goggles.visible = s.goggles;
    this.lamp.visible = s.lampOn;
    this.bottle.visible = s.bottles > 0;
    this.axe.visible = s.mode !== 'rest';
  }
}

export const SUIT_NAMES = SUITS.map((s) => s.name);
