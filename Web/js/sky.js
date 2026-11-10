// Sky dome (sun, moon, stars, high cloud) and the valley cloud layer.

import * as THREE from '../vendor/three.module.min.js';
import { CLOUD_FRAGMENT, CLOUD_VERTEX, SKY_FRAGMENT, SKY_VERTEX } from './shaders.js';

export class Sky {
  constructor(env, noiseTexture, terrainUniforms) {
    this.group = new THREE.Group();

    const dome = new THREE.Mesh(
      new THREE.IcosahedronGeometry(1, 4),
      new THREE.ShaderMaterial({
        uniforms: { ...env, uNoise: { value: noiseTexture } },
        vertexShader: SKY_VERTEX,
        fragmentShader: SKY_FRAGMENT,
        side: THREE.BackSide,
        depthWrite: false,
        depthTest: true,
      }),
    );
    dome.frustumCulled = false;
    dome.renderOrder = -10;
    this.group.add(dome);

    // Three stacked sheets give the cloud layer some depth when the camera moves through it.
    this.layers = [];
    const sheet = new THREE.PlaneGeometry(1, 1, 1, 1).rotateX(-Math.PI / 2);
    for (let i = 0; i < 3; i++) {
      const material = new THREE.ShaderMaterial({
        uniforms: { ...terrainUniforms, uLayer: { value: i } },
        vertexShader: CLOUD_VERTEX,
        fragmentShader: CLOUD_FRAGMENT,
        transparent: true,
        depthWrite: false,
        side: THREE.DoubleSide,
      });
      const mesh = new THREE.Mesh(sheet, material);
      mesh.scale.set(160000, 1, 160000);
      mesh.frustumCulled = false;
      mesh.renderOrder = 50 + i;
      this.group.add(mesh);
      this.layers.push(mesh);
    }
    this.env = env;
  }

  // The valley cloud deck rises through the day as the sun heats the slopes, a daily rhythm in the
  // Khumbu: clear mornings, cloud boiling up the valleys by afternoon.
  update(camera, hour, cloudCover) {
    const afternoon = Math.max(0, Math.sin(((hour - 8) / 12) * Math.PI));
    const base = 5150 + 700 * afternoon + 500 * cloudCover;
    const thickness = 260 + 420 * cloudCover;
    this.env.uCloudBase.value = base;
    this.env.uCloudTop.value = base + thickness;
    this.layers.forEach((mesh, i) => {
      mesh.position.set(camera.position.x, base + (thickness * (i + 0.5)) / 3, camera.position.z);
    });
  }
}
