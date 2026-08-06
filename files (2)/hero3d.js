// ============================================================
// ATLAS hero — a 3D ladder-logic network, receding into depth,
// with current pulses travelling along energized rungs.
// Built with three.js. No post-processing deps: glow is faked
// with additive-blended sprites generated on a canvas texture.
// ============================================================

(function () {
  const canvas = document.getElementById('hero-canvas');
  if (!canvas || typeof THREE === 'undefined') return;

  const prefersReduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  const scene = new THREE.Scene();
  scene.fog = new THREE.FogExp2(0x0a0c0f, 0.028);

  const camera = new THREE.PerspectiveCamera(50, window.innerWidth / window.innerHeight, 0.1, 200);
  camera.position.set(0, 1.6, 15);

  const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: true });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  renderer.setSize(window.innerWidth, window.innerHeight);

  // ---------- glow sprite texture ----------
  function makeGlowTexture(hex) {
    const size = 128;
    const c = document.createElement('canvas');
    c.width = c.height = size;
    const ctx = c.getContext('2d');
    const grad = ctx.createRadialGradient(size/2, size/2, 0, size/2, size/2, size/2);
    grad.addColorStop(0, hex + 'ff');
    grad.addColorStop(0.25, hex + 'aa');
    grad.addColorStop(1, hex + '00');
    ctx.fillStyle = grad;
    ctx.fillRect(0, 0, size, size);
    return new THREE.CanvasTexture(c);
  }
  const amberGlow = makeGlowTexture('#ff9e1b');
  const tealGlow  = makeGlowTexture('#2bd4a8');

  // ---------- ladder network geometry ----------
  const RUNGS = 14;
  const RAIL_X = [-6, 6];
  const Z_STEP = -2.1;
  const group = new THREE.Group();
  scene.add(group);

  const railMat = new THREE.LineBasicMaterial({ color: 0x2a3038, transparent: true, opacity: 0.55 });
  const rungMatCold = new THREE.LineBasicMaterial({ color: 0x333a43, transparent: true, opacity: 0.5 });
  const rungMatHot  = new THREE.LineBasicMaterial({ color: 0xff9e1b, transparent: true, opacity: 0.9 });

  // rails (two long verticals running the depth of the scene)
  RAIL_X.forEach((x) => {
    const pts = [];
    for (let i = -2; i <= RUNGS; i++) pts.push(new THREE.Vector3(x, 3, i * Z_STEP));
    for (let i = RUNGS; i >= -2; i--) pts.push(new THREE.Vector3(x, -3, i * Z_STEP));
    const geo = new THREE.BufferGeometry().setFromPoints(pts);
    group.add(new THREE.Line(geo, railMat));
  });

  const rungs = []; // {z, energized(bool), line, contacts:[sprite,sprite], pulse:sprite, phase}
  const contactColdGeo = null;

  for (let i = 0; i < RUNGS; i++) {
    const z = i * Z_STEP;
    const y = Math.sin(i * 0.7) * 1.1; // gentle vertical wander so it doesn't feel like a flat grid
    const energized = i % 3 !== 1; // majority "energized" look, some dark rungs for contrast

    const pts = [ new THREE.Vector3(RAIL_X[0], y, z), new THREE.Vector3(RAIL_X[1], y, z) ];
    const geo = new THREE.BufferGeometry().setFromPoints(pts);
    const line = new THREE.Line(geo, energized ? rungMatHot.clone() : rungMatCold.clone());
    group.add(line);

    // contact nodes at both ends
    const spriteMat = new THREE.SpriteMaterial({
      map: energized ? amberGlow : tealGlow,
      transparent: true, depthWrite: false, blending: THREE.AdditiveBlending,
      opacity: energized ? 0.9 : 0.35
    });
    const c1 = new THREE.Sprite(spriteMat);
    const c2 = new THREE.Sprite(spriteMat.clone());
    c1.position.set(RAIL_X[0], y, z); c1.scale.setScalar(0.5);
    c2.position.set(RAIL_X[1], y, z); c2.scale.setScalar(0.5);
    group.add(c1, c2);

    // travelling pulse along energized rungs
    let pulse = null;
    if (energized) {
      const pMat = new THREE.SpriteMaterial({ map: amberGlow, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, opacity: 1 });
      pulse = new THREE.Sprite(pMat);
      pulse.scale.setScalar(0.7);
      pulse.position.set(RAIL_X[0], y, z);
      group.add(pulse);
    }

    rungs.push({ z, y, energized, pulse, phase: Math.random() });
  }

  // ambient particulate dust for depth
  const dustCount = 220;
  const dustGeo = new THREE.BufferGeometry();
  const dustPos = new Float32Array(dustCount * 3);
  for (let i = 0; i < dustCount; i++) {
    dustPos[i*3]   = (Math.random() - 0.5) * 26;
    dustPos[i*3+1] = (Math.random() - 0.5) * 12;
    dustPos[i*3+2] = (Math.random()) * -30;
  }
  dustGeo.setAttribute('position', new THREE.BufferAttribute(dustPos, 3));
  const dustMat = new THREE.PointsMaterial({ color: 0x4a5560, size: 0.03, transparent: true, opacity: 0.5 });
  const dust = new THREE.Points(dustGeo, dustMat);
  scene.add(dust);

  // ---------- resize ----------
  function onResize() {
    camera.aspect = window.innerWidth / window.innerHeight;
    camera.updateProjectionMatrix();
    renderer.setSize(window.innerWidth, window.innerHeight);
  }
  window.addEventListener('resize', onResize);

  // ---------- pointer parallax ----------
  let targetRotY = 0, targetRotX = 0;
  window.addEventListener('pointermove', (e) => {
    targetRotY = (e.clientX / window.innerWidth - 0.5) * 0.25;
    targetRotX = (e.clientY / window.innerHeight - 0.5) * 0.12;
  });

  // ---------- animate ----------
  const clock = new THREE.Clock();
  let started = false;

  function animate() {
    requestAnimationFrame(animate);
    const t = clock.getElapsedTime();
    const dt = clock.getDelta();

    if (!started) { canvas.classList.add('ready'); started = true; }

    if (!prefersReduced) {
      group.rotation.y += (targetRotY - group.rotation.y) * 0.03;
      group.rotation.x += (targetRotX - group.rotation.x) * 0.03;
      group.position.z = ((t * 0.6) % Z_STEP) - Z_STEP; // slow drift through the network

      rungs.forEach((r) => {
        if (!r.pulse) return;
        const speed = 0.35;
        const p = ((t * speed) + r.phase) % 1;
        r.pulse.position.x = RAIL_X[0] + (RAIL_X[1] - RAIL_X[0]) * p;
        r.pulse.material.opacity = 0.6 + Math.sin(p * Math.PI) * 0.4;
      });

      dust.rotation.y += dt * 0.01;
    }

    camera.position.y = 1.6 + Math.sin(t * 0.15) * 0.15;
    camera.lookAt(0, 0, -8);

    renderer.render(scene, camera);
  }
  animate();
})();
