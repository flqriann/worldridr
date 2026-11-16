// Sky dome, sun/moon, ambient light, fog and environment reflections, all
// driven by the Lighting service (and its Sky / Atmosphere children).

import * as THREE from 'three';
import type { DataModel, Instance } from '../core/Instance';

const SKY_VERT = /* glsl */ `
varying vec3 vDir;
void main() {
  vDir = normalize((modelMatrix * vec4(position, 0.0)).xyz);
  vec4 p = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
  gl_Position = p.xyww;
}`;
const SKY_FRAG = /* glsl */ `
uniform vec3 topColor; uniform vec3 horizonColor; uniform vec3 groundColor;
uniform vec3 sunDir; uniform float sunSize; uniform float sunVisible; uniform float stars; uniform float day;
varying vec3 vDir;
float hash(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
void main() {
  vec3 d = normalize(vDir);
  float h = d.y;
  vec3 col = h > 0.0 ? mix(horizonColor, topColor, pow(h, 0.55)) : mix(horizonColor, groundColor, pow(min(-h * 4.0, 1.0), 0.6));
  float sd = dot(d, normalize(sunDir));
  float r = 0.9995 - sunSize * 0.0012;
  if (sunVisible > 0.5) {
    col += vec3(1.0, 0.92, 0.75) * smoothstep(r, r + 0.0004, sd) * 3.0 * max(day, 0.15);
    col += vec3(1.0, 0.75, 0.45) * pow(max(sd, 0.0), 180.0) * 0.6 * day;
    float md = dot(d, -normalize(sunDir));
    col += vec3(0.75, 0.8, 0.95) * smoothstep(0.9993, 0.9996, md) * (1.0 - day);
  }
  if (stars > 0.0 && h > 0.0) {
    vec3 cell = floor(d * 220.0);
    float s = hash(cell);
    col += vec3(step(0.9975, s) * stars * (0.6 + 0.4 * hash(cell + 3.1)));
  }
  gl_FragColor = vec4(col, 1.0);
  #include <colorspace_fragment>
}`;

export class Environment {
  readonly sky: THREE.Mesh;
  readonly sun = new THREE.DirectionalLight(0xffffff, 3);
  readonly hemi = new THREE.HemisphereLight(0xbfd4ff, 0x404040, 1);
  readonly ambient = new THREE.AmbientLight(0xffffff, 0.3);
  private skyUniforms = {
    topColor: { value: new THREE.Color() }, horizonColor: { value: new THREE.Color() }, groundColor: { value: new THREE.Color() },
    sunDir: { value: new THREE.Vector3(0, 1, 0) }, sunSize: { value: 1 }, sunVisible: { value: 1 }, stars: { value: 0 }, day: { value: 1 },
  };
  private pmrem: THREE.PMREMGenerator;
  private envScene = new THREE.Scene();
  private envRT: THREE.WebGLRenderTarget | null = null;
  private envDirty = true;
  private lastEnvTime = 0;
  private dm: DataModel | null = null;
  private unsub: (() => void)[] = [];
  private dirty = true;

  constructor(private scene: THREE.Scene, private renderer: THREE.WebGLRenderer) {
    const mat = new THREE.ShaderMaterial({ vertexShader: SKY_VERT, fragmentShader: SKY_FRAG, uniforms: this.skyUniforms, side: THREE.BackSide, depthWrite: false, fog: false });
    this.sky = new THREE.Mesh(new THREE.SphereGeometry(1, 48, 24), mat);
    this.sky.scale.setScalar(5000);
    this.sky.frustumCulled = false;
    this.sky.renderOrder = -1000;
    scene.add(this.sky, this.sun, this.sun.target, this.hemi, this.ambient);
    this.sun.castShadow = true;
    this.sun.shadow.mapSize.set(2048, 2048);
    const sc = this.sun.shadow.camera;
    sc.left = -140; sc.right = 140; sc.top = 140; sc.bottom = -140; sc.near = 1; sc.far = 1200;
    this.sun.shadow.bias = -0.0004;
    this.sun.shadow.normalBias = 0.04;
    this.pmrem = new THREE.PMREMGenerator(renderer);
    const envSky = new THREE.Mesh(this.sky.geometry, mat);
    envSky.scale.setScalar(100);
    this.envScene.add(envSky);
  }

  bind(dm: DataModel) {
    this.unsub.forEach((u) => u());
    this.dm = dm;
    const isEnv = (i: Instance) => i.className === 'Lighting' || ((i.className === 'Sky' || i.className === 'Atmosphere') && i.parent?.className === 'Lighting');
    const mark = (i: Instance) => { if (isEnv(i)) this.dirty = true; };
    this.unsub = [
      dm.changed.connect((i) => mark(i)),
      dm.added.connect((i) => mark(i)),
      dm.removed.connect((i) => { if (i.className === 'Sky' || i.className === 'Atmosphere') this.dirty = true; }),
      dm.reparented.connect(() => { this.dirty = true; }),
    ];
    this.dirty = true;
  }

  /** Recomputes lighting if anything changed; `focus` is where shadows are centered. */
  update(focus: THREE.Vector3) {
    // keep the shadow frustum around the camera focus
    const dir = this.skyUniforms.sunDir.value;
    const snap = (v: number) => Math.round(v / 4) * 4;
    this.sun.target.position.set(snap(focus.x), snap(focus.y), snap(focus.z));
    this.sun.position.copy(this.sun.target.position).addScaledVector(dir.y > 0 ? dir : dir.clone().negate(), 500);
    this.sun.target.updateMatrixWorld();

    if (this.dirty) { this.dirty = false; this.apply(); }
    const now = performance.now();
    if (this.envDirty && now - this.lastEnvTime > 400) {
      this.envDirty = false;
      this.lastEnvTime = now;
      const rt = this.pmrem.fromScene(this.envScene, 0, 0.1, 1000);
      this.envRT?.dispose();
      this.envRT = rt;
      this.scene.environment = rt.texture;
      this.scene.environmentIntensity = 0.6;
    }
  }

  private apply() {
    const dm = this.dm;
    if (!dm) return;
    const L = dm.getService('Lighting');
    if (!L) return;
    const sky = L.findFirstChildOfClass('Sky');
    const atm = L.findFirstChildOfClass('Atmosphere');
    const clock = ((L.props.ClockTime as number) % 24 + 24) % 24;
    // Sun travels east (+X) -> west (-X), tilted toward south (+Z) like a mid-latitude sky.
    const ang = ((clock - 6) / 12) * Math.PI;
    const sunDir = new THREE.Vector3(Math.cos(ang), Math.sin(ang), 0.35).normalize();
    const day = THREE.MathUtils.smoothstep(sunDir.y, -0.12, 0.25);
    const bright = L.props.Brightness as number;

    const u = this.skyUniforms;
    u.sunDir.value.copy(sunDir);
    u.day.value = day;
    const top = new THREE.Color(sky ? (sky.props.TopColor as string) : '#3d7fd6');
    const hor = new THREE.Color(sky ? (sky.props.HorizonColor as string) : '#c4e1ff');
    const gnd = new THREE.Color(sky ? (sky.props.GroundColor as string) : '#5f6168');
    const nightTop = new THREE.Color('#03060f'), nightHor = new THREE.Color('#0f1830');
    const sunset = THREE.MathUtils.clamp(1 - Math.abs(sunDir.y) * 4, 0, 1) * (sunDir.y > -0.15 ? 1 : 0);
    u.topColor.value.copy(nightTop).lerp(top, day);
    u.horizonColor.value.copy(nightHor).lerp(hor, day).lerp(new THREE.Color('#ff9a5c'), sunset * 0.45);
    u.groundColor.value.copy(gnd).multiplyScalar(0.3 + 0.7 * day);
    u.sunSize.value = sky ? (sky.props.SunSize as number) : 1;
    u.sunVisible.value = !sky || sky.props.SunVisible ? 1 : 0;
    u.stars.value = (!sky || sky.props.StarsVisible) ? (1 - day) : 0;
    this.sky.visible = true;

    const sunColor = new THREE.Color('#fff4e0').lerp(new THREE.Color('#ffb070'), sunset * 0.7);
    const moon = 1 - day;
    if (day > 0.02) {
      this.sun.color.copy(sunColor);
      this.sun.intensity = bright * 1.6 * day;
    } else {
      this.sun.color.set('#9fb4ff');
      this.sun.intensity = bright * 0.25 * moon;
    }
    this.sun.castShadow = !!L.props.GlobalShadows;
    const outdoor = new THREE.Color(L.props.OutdoorAmbient as string);
    this.hemi.color.copy(outdoor).lerp(u.topColor.value, 0.3);
    this.hemi.groundColor.copy(u.groundColor.value);
    this.hemi.intensity = 0.55 + 0.6 * day;
    this.ambient.color.set(L.props.Ambient as string);
    this.ambient.intensity = 0.6;
    this.renderer.toneMappingExposure = Math.pow(2, L.props.ExposureCompensation as number);

    const fogEnd = L.props.FogEnd as number;
    if (fogEnd < 99999) {
      this.scene.fog = new THREE.Fog(new THREE.Color(L.props.FogColor as string), L.props.FogStart as number, Math.max(fogEnd, (L.props.FogStart as number) + 1));
    } else if (atm && (atm.props.Density as number) > 0) {
      const c = new THREE.Color(atm.props.Color as string).lerp(u.horizonColor.value, 0.5).multiplyScalar(0.25 + 0.75 * day);
      this.scene.fog = new THREE.FogExp2(c, (atm.props.Density as number) * 0.0035);
    } else {
      this.scene.fog = null;
    }
    this.envDirty = true;
  }

  dispose() {
    this.unsub.forEach((u) => u());
    this.envRT?.dispose();
    this.pmrem.dispose();
  }
}
