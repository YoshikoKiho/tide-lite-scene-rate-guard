(() => {
  'use strict';

  const BIN = '/system/bin/sh /data/adb/modules/scene_rate_guard/bin/scene_guard.sh';
  const SCENE_PROFILE = '/data/data/com.omarea.vtools/files/profile.json';
  const LS = {
    theme: 'tide.lite.theme',
    wallpaper: 'tide.lite.wallpaper',
    font: 'tide.lite.font',
    customWall: 'tide.lite.customWall',
    sync: 'tide.lite.sync',
    demo: 'tide.lite.demo'
  };
  const DEFAULTS = { theme: 'lily', wallpaper: 'garden', font: 'serif' };
  const THEME_COLORS = {
    lily: '#e9efe9',
    sage: '#eef0e8',
    rose: '#f4ecec',
    wheat: '#f5efe4',
    dusk: '#eeecf4',
    glass: '#07110f'
  };
  const RATE_MAX = 20000;
  const RATE_POS_MAX = 1000;
  const RATE_CURVE = [
    [0, 0], [100, 200], [500, 400], [1000, 500],
    [2000, 620], [5000, 780], [10000, 890], [20000, 1000]
  ];
  const SCENE_NAMES = {
    powersave: '省电',
    balance: '均衡',
    performance: '性能',
    fast: '极速',
    pedestal: '基座'
  };
  const CLUSTER_NAMES = {
    1: ['全局策略'],
    2: ['能效核', '性能核'],
    3: ['能效核', '性能核', '超大核']
  };
  const CLUSTER_KINDS = ['LITTLE', 'PERFORMANCE', 'PRIME', 'POLICY'];

  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => Array.from(root.querySelectorAll(selector));

  const state = {
    kv: null,
    clusters: [],
    cards: [],
    signature: '',
    dirty: false,
    busy: false,
    demo: false,
    sync: false,
    scenePresets: [],
    refreshTimer: 0,
    toastTimer: 0,
    lastError: ''
  };

  const els = {
    grid: $('#clusterGrid'),
    guardPill: $('#guardPill'),
    guardText: $('#guardText'),
    chargeText: $('#chargeText'),
    syncText: $('#syncText'),
    heroValue: $('#heroValue'),
    deviceLine: $('#deviceLine'),
    dirtyLabel: $('#dirtyLabel'),
    applyButton: $('#applyButton'),
    pauseButton: $('#pauseButton'),
    syncToggle: $('#syncToggle'),
    demoBanner: $('#demoBanner'),
    toast: $('#toast'),
    sheet: $('#sheet'),
    themeGrid: $('#themeGrid'),
    wallpaperGrid: $('#wallpaperGrid'),
    fontGrid: $('#fontGrid'),
    customWall: $('#customWall'),
    upload: $('#wallpaperUpload'),
    logBox: $('#logBox'),
    diagGrid: $('#diagGrid'),
    nodeList: $('#nodeList'),
    sceneCard: $('#sceneCard'),
    scenePresets: $('#scenePresets'),
    sceneHint: $('#sceneHint'),
    sceneReload: $('#sceneReload')
  };

  /* ---------- utilities ---------- */

  function setText(node, text) {
    if (node) node.textContent = text;
  }

  function parseInt10(value) {
    const n = Number.parseInt(String(value).replace(/[^\d]/g, ''), 10);
    return Number.isFinite(n) ? n : 0;
  }

  function clamp(value, min, max) {
    return Math.max(min, Math.min(max, value));
  }

  function clampRate(value) {
    return clamp(Math.round(Number(value) || 0), 0, RATE_MAX);
  }

  function parseKV(text) {
    const out = {};
    String(text || '').replace(/\r/g, '').split('\n').forEach((line) => {
      const at = line.indexOf('=');
      if (at > 0) out[line.slice(0, at)] = line.slice(at + 1);
    });
    return out;
  }

  function safeGet(key) {
    try { return window.localStorage.getItem(key); } catch (err) { return null; }
  }

  function safeSet(key, value) {
    try {
      window.localStorage.setItem(key, value);
      return true;
    } catch (err) {
      toast('本地存储空间不足，请换一张更小的壁纸', true);
      return false;
    }
  }

  function toast(message, isError = false) {
    const node = els.toast;
    if (!node) return;
    node.textContent = message;
    node.classList.toggle('is-error', isError);
    node.classList.add('is-show');
    window.clearTimeout(state.toastTimer);
    state.toastTimer = window.setTimeout(() => node.classList.remove('is-show'), 2800);
  }

  function formatWhen(epoch) {
    const n = Number(epoch);
    if (!Number.isFinite(n) || n <= 0) return '尚未校准';
    const date = new Date(n * 1000);
    const diff = Date.now() - date.getTime();
    if (diff >= 0 && diff < 45000) return '刚刚';
    const hh = String(date.getHours()).padStart(2, '0');
    const mm = String(date.getMinutes()).padStart(2, '0');
    const sameDay = new Date().toDateString() === date.toDateString();
    return sameDay ? `${hh}:${mm}` : `${date.getMonth() + 1}-${date.getDate()} ${hh}:${mm}`;
  }

  function cpuLabel(raw) {
    const text = String(raw || '').trim();
    if (text === 'all') return '全部 CPU';
    if (!text) return 'CPU —';
    const expanded = text.replace(/(\d+)\s*[-–]\s*(\d+)/g, (_, a, b) => {
      const from = Number(a);
      const to = Number(b);
      if (!Number.isFinite(from) || !Number.isFinite(to) || to < from || to - from > 64) return `${a} ${b}`;
      const list = [];
      for (let n = from; n <= to; n += 1) list.push(n);
      return list.join(' ');
    });
    const nums = expanded.split(/[\s,]+/).map((x) => parseInt10(x)).filter((n) => n >= 0).sort((a, b) => a - b);
    if (!nums.length) return 'CPU —';
    const ranges = [];
    let start = nums[0];
    let prev = nums[0];
    for (let i = 1; i <= nums.length; i += 1) {
      const n = nums[i];
      if (n !== prev + 1) {
        ranges.push(start === prev ? String(start) : `${start}–${prev}`);
        start = n;
      }
      prev = n;
    }
    return `CPU ${ranges.join(', ')}`;
  }

  function firstCpu(raw) {
    const nums = String(raw || '').split(/[\s,]+/).map((x) => parseInt10(x)).filter((n) => n >= 0);
    return nums.length ? nums[0] : 0;
  }

  function toMHz(khz) {
    const n = Number(khz);
    return Number.isFinite(n) ? String(Math.round(n / 1000)) : '—';
  }

  function avgRate(cards) {
    if (!cards.length) return '—';
    const values = cards.map((card) => Number(card.rate)).filter(Number.isFinite);
    return values.length ? String(Math.round(values.reduce((a, b) => a + b, 0) / values.length)) : '—';
  }

  function rateToPos(rate) {
    const value = clampRate(rate);
    for (let i = 0; i < RATE_CURVE.length - 1; i += 1) {
      const [v0, p0] = RATE_CURVE[i];
      const [v1, p1] = RATE_CURVE[i + 1];
      if (value <= v1) {
        const ratio = v1 === v0 ? 0 : (value - v0) / (v1 - v0);
        return Math.round(p0 + (p1 - p0) * ratio);
      }
    }
    return RATE_POS_MAX;
  }

  function posToRate(pos) {
    const p = clamp(Number(pos) || 0, 0, RATE_POS_MAX);
    for (let i = 0; i < RATE_CURVE.length - 1; i += 1) {
      const [v0, p0] = RATE_CURVE[i];
      const [v1, p1] = RATE_CURVE[i + 1];
      if (p <= p1) {
        const ratio = p1 === p0 ? 0 : (p - p0) / (p1 - p0);
        return Math.round(v0 + (v1 - v0) * ratio);
      }
    }
    return RATE_MAX;
  }

  /* ---------- press animation (no vibration) ---------- */  let pressedButton = null;

  function pressStart(event) {
    const button = event.target.closest('button, .upload-button');
    if (!button || button.disabled) return;
    pressedButton = button;
    button.classList.remove('is-bounced');
    button.classList.add('is-pressed');
  }

  function pressEnd() {
    const button = pressedButton;
    pressedButton = null;
    if (!button) return;
    button.classList.remove('is-pressed');
    button.classList.remove('is-bounced');
    void button.offsetWidth;
    button.classList.add('is-bounced');
    window.setTimeout(() => button.classList.remove('is-bounced'), 380);
  }

  /* ---------- rate slider: per-frame paint, no vibration ---------- */

  function attachSlider(input, bubble, options = {}) {
    const min = Number(input.min) || 0;
    const max = Number(input.max) || 100;
    const toValue = options.toValue || ((raw) => raw);
    const format = options.format || ((value) => String(value));
    let hideTimer = 0;
    let frame = 0;
    let pending = null;
    const host = input.closest('.slider');

    function paint(raw) {
      const actual = toValue(raw);
      const pct = max === min ? 0 : (clamp(raw, min, max) - min) / (max - min);
      input.style.setProperty('--fill', `${pct * 100}%`);
      bubble.textContent = format(actual);
      bubble.style.left = `clamp(30px, calc(${pct * 100}% + ${(0.5 - pct) * 22}px), calc(100% - 30px))`;
    }

    function schedule(raw) {
      pending = raw;
      if (frame) return;
      frame = window.requestAnimationFrame(() => {
        frame = 0;
        if (pending !== null) paint(pending);
      });
    }

    function show() {
      if (!host) return;
      window.clearTimeout(hideTimer);
      host.classList.add('is-dragging');
      paint(Number(input.value));
    }

    function hideSoon() {
      window.clearTimeout(hideTimer);
      hideTimer = window.setTimeout(() => {
        if (host && document.activeElement !== input) host.classList.remove('is-dragging');
      }, 260);
    }

    input.addEventListener('pointerdown', show, { passive: true });
    input.addEventListener('pointerup', hideSoon, { passive: true });
    input.addEventListener('pointercancel', hideSoon, { passive: true });
    input.addEventListener('focus', show);
    input.addEventListener('blur', hideSoon);
    input.addEventListener('input', () => {
      const raw = Number(input.value);
      schedule(raw);
      if (options.onInput) options.onInput(raw, toValue(raw));
    });
    input.addEventListener('change', hideSoon);
    paint(Number(input.value));

    return {
      paint: () => paint(Number(input.value)),
      setValue(raw) {
        input.value = String(raw);
        paint(Number(input.value));
      }
    };
  }

  /* ---------- KernelSU bridge ---------- */

  function hasKsu() {
    return Boolean(window.ksu && typeof window.ksu.exec === 'function');
  }

  function ksuExec(command) {
    if (!hasKsu()) return demoExec(command);
    return new Promise((resolve, reject) => {
      const name = `__tide_cb_${Date.now().toString(36)}_${Math.random().toString(36).slice(2)}`;
      let finished = false;
      let timer = 0;

      const cleanup = () => {
        window.clearTimeout(timer);
        try { delete window[name]; } catch (err) { window[name] = undefined; }
      };

      const finish = (errno, stdout, stderr) => {
        if (finished) return;
        finished = true;
        cleanup();
        resolve({
          errno: Number(errno) || 0,
          stdout: stdout == null ? '' : String(stdout),
          stderr: stderr == null ? '' : String(stderr)
        });
      };

      const fail = (error) => {
        if (finished) return;
        finished = true;
        cleanup();
        reject(error instanceof Error ? error : new Error(String(error)));
      };

      timer = window.setTimeout(() => fail(new Error('KernelSU 响应超时')), 15000);
      window[name] = finish;

      try {
        const ret = window.ksu.exec(command, '{}', name);
        if (ret && typeof ret.then === 'function') {
          ret.then((value) => {
            if (!value) return finish(0, '', '');
            if (typeof value === 'object' && ('stdout' in value || 'errno' in value)) {
              finish(value.errno, value.stdout, value.stderr);
            } else {
              finish(0, String(value), '');
            }
          }).catch(fail);
        }
      } catch (err) {
        try {
          window.ksu.exec(command, name);
        } catch (err2) {
          fail(err2 || err);
        }
      }
    });
  }

  async function fetchStatus() {
    const res = await ksuExec(`${BIN} status`);
    const kv = parseKV(res.stdout);
    if (!Object.keys(kv).length) throw new Error(res.stderr.trim() || '模块没有返回状态');
    if (res.errno && kv.ok !== '0') throw new Error(res.stderr.trim() || `脚本退出码 ${res.errno}`);
    return kv;
  }

  async function runCommand(args) {
    const res = await ksuExec(`${BIN} ${args.join(' ')}`);
    const kv = parseKV(res.stdout);
    if (!Object.keys(kv).length) throw new Error(res.stderr.trim() || '模块没有返回结果');
    if (kv.ok !== '1') throw new Error(ERROR_TEXT[kv.error] || kv.error || res.stderr.trim() || '操作失败');
    return kv;
  }

  async function fetchLog() {
    const res = await ksuExec(`${BIN} log`);
    if (res.errno && !res.stdout.trim()) throw new Error(res.stderr.trim() || '无法读取日志');
    return res.stdout;
  }

  async function fetchSceneProfile() {
    if (!hasKsu()) return JSON.stringify(DEMO_SCENE_PROFILE);
    const res = await ksuExec(`cat ${SCENE_PROFILE} 2>/dev/null`);
    if (res.errno || !res.stdout.trim()) throw new Error('未找到 Scene profile.json');
    return res.stdout;
  }

  const ERROR_TEXT = {
    no_rate_nodes: '没有找到 up/down rate_limit_us 节点',
    count_mismatch: '集群数量变化，请刷新',
    invalid_rate: '限幅值超出 0–20000',
    config_write_failed: '无法保存配置',
    write_failed: '内核拒绝了写入，请查看日志'
  };

  /* ---------- demo backend ---------- */

  const DEMO_SCENE_PROFILE = {
    schemes: {
      powersave: { call: [
        ['/sys/devices/system/cpu/cpu0/cpufreq/sugov_ext/up_rate_limit_us', '16000'],
        ['/sys/devices/system/cpu/cpu0/cpufreq/sugov_ext/down_rate_limit_us', '16000'],
        ['/sys/devices/system/cpu/cpu4/cpufreq/sugov_ext/up_rate_limit_us', '12000'],
        ['/sys/devices/system/cpu/cpu4/cpufreq/sugov_ext/down_rate_limit_us', '12000'],
        ['/sys/devices/system/cpu/cpu7/cpufreq/sugov_ext/up_rate_limit_us', '4000'],
        ['/sys/devices/system/cpu/cpu7/cpufreq/sugov_ext/down_rate_limit_us', '4000']
      ] },
      balance: { call: [
        ['/sys/devices/system/cpu/cpu0/cpufreq/sugov_ext/up_rate_limit_us', '8000'],
        ['/sys/devices/system/cpu/cpu0/cpufreq/sugov_ext/down_rate_limit_us', '8000'],
        ['/sys/devices/system/cpu/cpu4/cpufreq/sugov_ext/up_rate_limit_us', '4000'],
        ['/sys/devices/system/cpu/cpu4/cpufreq/sugov_ext/down_rate_limit_us', '4000'],
        ['/sys/devices/system/cpu/cpu7/cpufreq/sugov_ext/up_rate_limit_us', '1000'],
        ['/sys/devices/system/cpu/cpu7/cpufreq/sugov_ext/down_rate_limit_us', '1000']
      ] },
      performance: { call: [
        ['/sys/devices/system/cpu/cpu0/cpufreq/sugov_ext/up_rate_limit_us', '2000'],
        ['/sys/devices/system/cpu/cpu0/cpufreq/sugov_ext/down_rate_limit_us', '2000'],
        ['/sys/devices/system/cpu/cpu4/cpufreq/sugov_ext/up_rate_limit_us', '1000'],
        ['/sys/devices/system/cpu/cpu4/cpufreq/sugov_ext/down_rate_limit_us', '1000'],
        ['/sys/devices/system/cpu/cpu7/cpufreq/sugov_ext/up_rate_limit_us', '400'],
        ['/sys/devices/system/cpu/cpu7/cpufreq/sugov_ext/down_rate_limit_us', '400']
      ] }
    }
  };

  const DEMO_BASE = {
    enabled: true,
    configured: true,
    daemon: true,
    charging: false,
    battery: 38,
    fixes: 9,
    last_apply: Math.floor(Date.now() / 1000) - 31,
    clusters: [
      { policy: 'policy0', cpus: '0 1 2 3', max: 2400000, gov: 'sugov_ext',
        upPath: '/sys/devices/system/cpu/cpufreq/policy0/sugov_ext/up_rate_limit_us',
        downPath: '/sys/devices/system/cpu/cpufreq/policy0/sugov_ext/down_rate_limit_us',
        up: 16000, down: 16000, desired: 16000 },
      { policy: 'policy4', cpus: '4 5 6', max: 3300000, gov: 'sugov_ext',
        upPath: '/sys/devices/system/cpu/cpufreq/policy4/sugov_ext/up_rate_limit_us',
        downPath: '/sys/devices/system/cpu/cpufreq/policy4/sugov_ext/down_rate_limit_us',
        up: 12000, down: 12000, desired: 12000 },
      { policy: 'policy7', cpus: '7', max: 3730000, gov: 'sugov_ext',
        upPath: '/sys/devices/system/cpu/cpufreq/policy7/sugov_ext/up_rate_limit_us',
        downPath: '/sys/devices/system/cpu/cpufreq/policy7/sugov_ext/down_rate_limit_us',
        up: 4000, down: 4000, desired: 4000 }
    ]
  };

  function loadDemo() {
    try {
      const raw = safeGet(LS.demo);
      if (raw) {
        const parsed = JSON.parse(raw);
        if (parsed && Array.isArray(parsed.clusters) && parsed.clusters.length) return parsed;
      }
    } catch (err) { /* use base */ }
    return JSON.parse(JSON.stringify(DEMO_BASE));
  }

  function saveDemo(value) {
    safeSet(LS.demo, JSON.stringify(value));
  }

  function demoStatus(demo) {
    const lines = [
      'ok=1',
      `enabled=${demo.enabled ? 1 : 0}`,
      'configured=1',
      `daemon=${demo.daemon ? 1 : 0}`,
      `charging=${demo.charging ? 1 : 0}`,
      `battery=${demo.battery}`,
      `fixes=${demo.fixes}`,
      `last_apply=${demo.last_apply}`,
      `count=${demo.clusters.length}`,
      'device=25060RK16C',
      'soc=MT6991',
      `max_rate=${RATE_MAX}`,
      'interval=2'
    ];
    demo.clusters.forEach((c, i) => {
      lines.push(`c${i}_policy=${c.policy}`);
      lines.push(`c${i}_cpus=${c.cpus}`);
      lines.push(`c${i}_max=${c.max}`);
      lines.push(`c${i}_gov=${c.gov}`);
      lines.push(`c${i}_up_path=${c.upPath}`);
      lines.push(`c${i}_down_path=${c.downPath}`);
      lines.push(`c${i}_up=${c.up}`);
      lines.push(`c${i}_down=${c.down}`);
      lines.push(`c${i}_desired=${c.desired}`);
    });
    return lines.join('\n') + '\n';
  }

  function demoExec(command) {
    return new Promise((resolve) => {
      window.setTimeout(() => {
        const demo = loadDemo();
        const args = String(command || '').split(/\s+/);

        if (command.includes(' status')) {
          resolve({ errno: 0, stdout: demoStatus(demo), stderr: '' });
          return;
        }

        if (command.includes(' profile.json')) {
          resolve({ errno: 0, stdout: JSON.stringify(DEMO_SCENE_PROFILE), stderr: '' });
          return;
        }

        if (command.includes(' apply')) {
          const start = args.indexOf('apply');
          const values = args.slice(start + 1).map(Number);
          if (values.length !== demo.clusters.length || values.some((n) => !Number.isFinite(n))) {
            resolve({ errno: 1, stdout: 'ok=0\nerror=count_mismatch\n', stderr: '' });
            return;
          }
          demo.clusters.forEach((c, i) => {
            c.desired = clampRate(values[i]);
            c.up = c.desired;
            c.down = c.desired;
          });
          demo.enabled = true;
          demo.fixes += 1;
          demo.last_apply = Math.floor(Date.now() / 1000);
          saveDemo(demo);
          resolve({ errno: 0, stdout: 'ok=1\nenabled=1\ncount=3\n', stderr: '' });
          return;
        }

        if (command.includes(' pause')) {
          demo.enabled = false;
          saveDemo(demo);
          resolve({ errno: 0, stdout: 'ok=1\nenabled=0\n', stderr: '' });
          return;
        }

        if (command.includes(' resume')) {
          demo.enabled = true;
          demo.fixes += 1;
          demo.last_apply = Math.floor(Date.now() / 1000);
          saveDemo(demo);
          resolve({ errno: 0, stdout: 'ok=1\nenabled=1\n', stderr: '' });
          return;
        }

        if (command.includes(' log')) {
          resolve({
            errno: 0,
            stdout: [
              '11-04 06:57:45 daemon started pid=22093',
              '11-04 06:57:45 rate policy0 10/10 -> 16000',
              '11-04 06:57:46 rate policy4 10/10 -> 12000',
              '11-04 06:57:46 rate policy7 10/10 -> 4000',
              '11-04 07:01:12 charger 1 -> 0, re-pin',
              '（预览模式，以上为示例日志）'
            ].join('\n'),
            stderr: ''
          });
          return;
        }

        resolve({ errno: 1, stdout: 'ok=0\nerror=invalid_rate\n', stderr: '' });
      }, 70);
    });
  }

  /* ---------- scene presets ---------- */

  function parseScenePresets(text) {
    let profile;
    try { profile = JSON.parse(text); } catch (err) { return []; }
    const out = [];
    Object.keys(profile.schemes || {}).forEach((key) => {
      const scheme = profile.schemes[key];
      const calls = scheme && Array.isArray(scheme.call) ? scheme.call : [];
      const byCpu = {};
      calls.forEach((call) => {
        if (!Array.isArray(call) || call.length < 2) return;
        const path = String(call[0]);
        const value = Number(call[1]);
        if (!Number.isFinite(value)) return;
        const match = path.match(/\/cpu(\d+)\/cpufreq\/[^/]+\/(up|down)_rate_limit_us$/);
        if (!match) return;
        const cpu = match[1];
        byCpu[cpu] = byCpu[cpu] || {};
        byCpu[cpu][match[2]] = value;
      });
      const cpus = Object.keys(byCpu).map(Number).sort((a, b) => a - b);
      if (!cpus.length) return;
      const values = cpus.map((cpu) => clampRate(byCpu[cpu].up ?? byCpu[cpu].down));
      out.push({
        key,
        name: SCENE_NAMES[key] || key,
        byCpu,
        values
      });
    });
    return out;
  }

  function renderScenePresets() {
    els.scenePresets.replaceChildren();
    if (!state.scenePresets.length) {
      els.sceneCard.hidden = true;
      return;
    }
    els.sceneCard.hidden = false;
    state.scenePresets.forEach((preset) => {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'scene-preset';
      button.dataset.scene = preset.key;
      const title = document.createElement('b');
      title.textContent = `Scene ${preset.name}`;
      const value = document.createElement('span');
      value.textContent = preset.values.join('/');
      button.append(title, value);
      button.addEventListener('click', () => applyScenePreset(preset));
      els.scenePresets.appendChild(button);
    });
  }

  function applyScenePreset(preset) {
    if (!state.cards.length) return;
    state.cards.forEach((card, i) => {
      const cpu = String(firstCpu(card.data.cpus));
      const entry = preset.byCpu[cpu];
      const fallback = preset.values[i];
      const value = clampRate(entry ? (entry.up ?? entry.down) : fallback);
      card.rate = value;
      paintRate(card, true);
    });
    state.dirty = true;
    updateDirty();
    $$('.scene-preset', els.scenePresets).forEach((button) => {
      button.setAttribute('aria-pressed', String(button.dataset.scene === preset.key));
    });
    toast(`已填入 Scene ${preset.name}，点「应用并守护」生效`);
  }

  async function loadScenePresets(showToast = false) {
    try {
      const text = await fetchSceneProfile();
      const presets = parseScenePresets(text);
      state.scenePresets = presets;
      renderScenePresets();
      if (showToast) toast(presets.length ? '已读取 Scene 方案' : 'Scene 配置里没有限幅方案');
    } catch (err) {
      state.scenePresets = [];
      els.sceneCard.hidden = true;
      if (showToast) toast('未找到 Scene 配置，可手动设置', true);
    }
  }

  /* ---------- data mapping ---------- */

  function clustersFrom(kv) {
    const count = Math.max(0, Math.min(8, parseInt10(kv.count)));
    const out = [];
    for (let i = 0; i < count; i += 1) {
      out.push({
        policy: String(kv[`c${i}_policy`] || `policy${i}`),
        cpus: String(kv[`c${i}_cpus`] || ''),
        max: parseInt10(kv[`c${i}_max`]),
        gov: String(kv[`c${i}_gov`] || ''),
        upPath: String(kv[`c${i}_up_path`] || ''),
        downPath: String(kv[`c${i}_down_path`] || ''),
        up: parseInt10(kv[`c${i}_up`]),
        down: parseInt10(kv[`c${i}_down`]),
        desired: clampRate(parseInt10(kv[`c${i}_desired`] ?? kv[`c${i}_up`] ?? 0))
      });
    }
    return out;
  }

  function clusterName(index, count) {
    const names = CLUSTER_NAMES[count];
    if (names && names[index]) return names[index];
    return `集群 ${index + 1}`;
  }

  /* ---------- card build ---------- */

  function buildCards(clusters) {
    els.grid.replaceChildren();
    state.cards = [];

    clusters.forEach((data, i) => {
      const article = document.createElement('article');
      article.className = 'cluster';
      article.dataset.index = String(i);
      article.innerHTML = `
        <div class="cluster-top">
          <span class="cluster-no"></span>
          <span class="cluster-kind"></span>
        </div>
        <div class="cluster-head">
          <div>
            <h3></h3>
            <div class="cluster-meta"><span class="cpus"></span><span class="maxfreq"></span></div>
          </div>
          <span class="cluster-live"><i class="live-dot"></i><span class="live-rate">—</span></span>
        </div>
        <div class="setting-block rate-block">
          <div class="block-head">
            <span>RATE LIMIT</span>
            <span class="kernel-state"><i></i><span class="rate-kernel">内核 —</span></span>
          </div>
          <div class="rate-readout">
            <div class="value-line">
              <input class="cluster-value" type="number" inputmode="numeric" min="0" max="${RATE_MAX}" step="10">
              <span class="value-unit">µs</span>
            </div>
          </div>
          <div class="slider">
            <input class="range rate-range" type="range" min="0" max="${RATE_POS_MAX}" step="1">
            <span class="range-bubble"></span>
            <div class="range-scale"><span>0</span><span>${RATE_MAX}</span></div>
          </div>
          <p class="rate-foot">同时写入 ↑ up_rate_limit_us / ↓ down_rate_limit_us</p>
        </div>
      `;

      const card = {
        root: article,
        index: i,
        data,
        rate: data.desired,
        input: $('.cluster-value', article),
        range: $('.rate-range', article),
        bubble: $('.rate-bubble', article),
        rateKernel: $('.rate-kernel', article),
        kernelState: $('.kernel-state', article),
        liveRate: $('.live-rate', article),
        slider: null
      };

      // rate-bubble lives inside the rate slider
      card.bubble = $('.rate-block .range-bubble', article);

      $('.cluster-no', article).textContent = String(i + 1).padStart(2, '0');
      $('.cluster-kind', article).textContent = CLUSTER_KINDS[i] || 'POLICY';
      $('.cluster h3', article).textContent = clusterName(i, clusters.length);
      $('.cpus', article).textContent = cpuLabel(data.cpus);
      $('.maxfreq', article).textContent = `上限 ${toMHz(data.max)} MHz`;
      card.input.setAttribute('aria-label', `${clusterName(i, clusters.length)} rate limit 微秒`);
      card.range.setAttribute('aria-label', `${clusterName(i, clusters.length)} rate limit 滑条`);

      card.slider = attachSlider(card.range, card.bubble, {
        format: (value) => `${value} µs`,
        toValue: posToRate,
        onInput: (raw, value) => {
          card.rate = clampRate(value);
          state.dirty = true;
          updateDirty();
          paintRate(card, true);
          if (state.sync) {
            state.cards.forEach((other) => {
              if (other !== card) {
                other.rate = card.rate;
                paintRate(other, true);
              }
            });
          }
        }
      });

      card.input.addEventListener('blur', () => {
        const raw = card.input.value.trim();
        card.rate = raw === '' ? 0 : clampRate(parseInt10(raw));
        card.input.value = String(card.rate);
        paintRate(card, false);
      });
      card.input.addEventListener('input', () => {
        const raw = card.input.value.trim();
        state.dirty = true;
        updateDirty();
        if (raw === '') return;
        card.rate = clampRate(parseInt10(raw));
        card.range.value = String(rateToPos(card.rate));
        card.slider.paint();
        paintRate(card, false, true);
        if (state.sync) {
          state.cards.forEach((other) => {
            if (other !== card) {
              other.rate = card.rate;
              paintRate(other, true);
            }
          });
        }
      });

      state.cards.push(card);
      els.grid.appendChild(article);
    });

    if (!clusters.length) {
      renderUnavailable(null);
      return;
    }
    state.cards.forEach((card) => paintCard(card));
  }

  function paintRate(card, writeInput = true, keepCursor = false) {
    const rate = clampRate(card.rate);
    card.rate = rate;
    if (writeInput && !(keepCursor && document.activeElement === card.input)) {
      card.input.value = String(rate);
    }
    card.input.style.width = `${Math.max(2.2, String(rate).length + 0.55)}ch`;
    card.range.value = String(rateToPos(rate));
    if (card.slider) card.slider.paint();

    const up = card.data.up;
    const down = card.data.down;
    const kernel = up === down ? `${up}` : `${up}/${down}`;
    card.rateKernel.textContent = `内核 ${kernel} µs`;
    card.kernelState.classList.toggle(
      'is-mismatch',
      Number(up) !== rate || Number(down) !== rate
    );
    card.liveRate.textContent = `${rate} µs`;
  }

  function paintCard(card) {
    setText($('.maxfreq', card.root), `上限 ${toMHz(card.data.max)} MHz`);
    paintRate(card, true, true);
  }

  function syncCard(card, data) {
    card.data = data;
    card.rate = data.desired;
    paintCard(card);
  }

  function mergeLive(card, data) {
    card.data = {
      ...card.data,
      up: data.up,
      down: data.down,
      gov: data.gov,
      max: data.max,
      cpus: data.cpus
    };
    paintCard(card);
  }

  function renderClusters(clusters) {
    const signature = clusters.map((c) => `${c.policy}|${c.cpus}|${c.max}|${c.upPath}|${c.downPath}`).join(';');
    if (signature !== state.signature) {
      state.signature = signature;
      buildCards(clusters);
      return;
    }
    clusters.forEach((data, i) => {
      const card = state.cards[i];
      if (!card) return;
      if (state.dirty) mergeLive(card, data);
      else syncCard(card, data);
    });
  }

  function renderUnavailable(kv) {
    state.signature = 'empty';
    state.cards = [];
    els.grid.replaceChildren();
    const box = document.createElement('div');
    box.className = 'cluster-empty';
    const title = document.createElement('b');
    title.textContent = '没有找到 up/down rate_limit_us 节点';
    box.appendChild(title);
    box.appendChild(document.createElement('br'));
    const copy = document.createElement('span');
    copy.textContent = '请确认当前 governor 为 sugov_ext，或节点路径与预期不同。';
    box.appendChild(copy);
    const code = document.createElement('code');
    code.textContent = 'find /sys/devices/system/cpu/cpufreq -name "*rate_limit_us"';
    box.appendChild(document.createElement('br'));
    box.appendChild(code);
    els.grid.appendChild(box);
    setText(els.heroValue, '—');
    if (kv && kv.error === 'no_rate_nodes') setText(els.guardText, '未发现节点');
  }

  function diagCard(label, value) {
    const div = document.createElement('div');
    div.className = 'diag-card';
    const small = document.createElement('small');
    small.textContent = label;
    const b = document.createElement('b');
    b.textContent = value;
    div.append(small, b);
    return div;
  }

  function renderDiag() {
    const kv = state.kv || {};
    const active = kv.enabled === '1';
    const daemon = kv.daemon === '1';
    els.diagGrid.replaceChildren(
      diagCard('GUARD', state.demo ? '预览' : (daemon ? (active ? '运行中' : '已暂停') : (active ? '未启动' : '已暂停'))),
      diagCard('POWER', kv.charging === '1' ? '电源接入' : '电池供电'),
      diagCard('BATTERY', `${kv.battery || '—'}%`),
      diagCard('RE-PINS', String(parseInt10(kv.fixes)))
    );

    els.nodeList.replaceChildren();
    state.cards.forEach((card) => {
      const row = document.createElement('div');
      row.className = 'node-item';
      const path = document.createElement('span');
      path.textContent = `${card.data.upPath || '—'} · ${card.data.downPath || '—'}`;
      const meta = document.createElement('span');
      meta.textContent = `${card.data.policy} · ${card.data.gov} · 目标 ${card.rate} µs`;
      const value = document.createElement('b');
      value.textContent = `${card.data.up}/${card.data.down} → ${card.rate}`;
      if (Number(card.data.up) !== card.rate || Number(card.data.down) !== card.rate) {
        row.classList.add('is-bad');
      }
      row.append(path, meta, value);
      els.nodeList.appendChild(row);
    });
  }

  function updateDirty() {
    const kv = state.kv || {};
    const active = kv.enabled === '1';
    const healthy = active && (kv.daemon === '1' || state.demo);
    setText(els.dirtyLabel, state.dirty ? '有未应用的修改' : (healthy ? '参数已同步' : (active ? '守护未运行' : '尚未启用')));
  }

  function renderStatus(kv) {
    const active = kv.enabled === '1';
    const daemon = kv.daemon === '1';
    const healthy = active && (daemon || state.demo);
    const demo = state.demo;
    const pillText = demo ? '预览模式' : (healthy ? '守护中' : (active ? '守护未运行' : (kv.configured === '1' ? '已暂停' : '待启用')));

    setText(els.guardPill.querySelector('span'), pillText);
    els.guardPill.classList.toggle('is-off', !healthy && !demo);
    els.guardPill.classList.toggle('is-demo', demo);

    setText(els.guardText, demo ? '预览运行' : (healthy ? '运行中' : (active ? '守护未运行' : (kv.configured === '1' ? '已暂停' : '待启用'))));
    const battery = kv.battery && kv.battery !== '' ? `${kv.battery}%` : '';
    setText(els.chargeText, kv.charging === '1' ? `充电中${battery ? ` · ${battery}` : ''}` : `电池${battery ? ` · ${battery}` : ''}`);
    setText(els.syncText, formatWhen(kv.last_apply));
    setText(els.heroValue, avgRate(state.cards));
    setText(els.pauseButton, active ? '暂停守护' : '恢复守护');
    els.pauseButton.disabled = !active && kv.configured !== '1';
    setText(els.applyButton.querySelector('span'), healthy ? '保存并校准' : '应用并守护');
    updateDirty();

    const soc = kv.soc && kv.soc !== 'unknown' ? kv.soc : 'MT6991';
    const device = kv.device || 'Redmi K80 Ultra';
    setText(els.deviceLine, `${device} · ${soc} · ${state.clusters.length} clusters`);

    const firstTag = $('.hero-tags span:first-child');
    if (firstTag) firstTag.textContent = soc === 'MT6991' ? '天玑 9400+' : soc;
    const countTag = $('.hero-tags span:nth-child(2)');
    if (countTag) countTag.textContent = `${state.clusters.length || 3} CLUSTERS`;

    if (!state.clusters.length) {
      setText(els.guardPill.querySelector('span'), '未发现节点');
      els.guardPill.classList.add('is-off');
      setText(els.guardText, '未发现节点');
    }

    renderDiag();
  }

  function renderConnectionError(err, notify) {
    const message = err instanceof Error ? err.message : String(err);
    state.lastError = message;
    setText(els.guardPill.querySelector('span'), '连接失败');
    els.guardPill.classList.add('is-off');
    setText(els.guardText, '无法读取');
    setText(els.chargeText, '未知');
    setText(els.syncText, '—');
    setText(els.dirtyLabel, '未连接');
    if (!state.clusters.length) {
      els.grid.replaceChildren();
      const box = document.createElement('div');
      box.className = 'cluster-empty';
      const b = document.createElement('b');
      b.textContent = '无法连接模块后端';
      box.append(b, document.createElement('br'), document.createTextNode(message));
      els.grid.appendChild(box);
    }
    if (notify) toast(message, true);
  }

  async function refresh(silent = false) {
    if (state.busy) return;
    try {
      const kv = await fetchStatus();
      state.kv = kv;
      const clusters = clustersFrom(kv);
      state.clusters = clusters;
      if (!clusters.length) renderUnavailable(kv);
      else renderClusters(clusters);
      renderStatus(kv);
    } catch (err) {
      renderConnectionError(err, !silent);
    }
  }

  /* ---------- actions ---------- */

  async function applyValues() {
    if (state.busy || !state.cards.length) {
      toast('还没有可写入的集群', true);
      return;
    }
    const values = state.cards.map((card) => clampRate(card.rate));
    state.busy = true;
    els.applyButton.disabled = true;
    els.applyButton.classList.add('is-loading');
    let success = false;
    try {
      await runCommand(['apply'].concat(values.map(String)));
      state.dirty = false;
      success = true;
      toast(state.demo ? '预览：参数已模拟应用' : '已应用，守护中');
    } catch (err) {
      toast(err instanceof Error ? err.message : String(err), true);
    } finally {
      state.busy = false;
      els.applyButton.disabled = false;
      els.applyButton.classList.remove('is-loading');
    }
    if (success) await refresh(true);
  }

  async function togglePause() {
    if (state.busy) return;
    const active = state.kv && state.kv.enabled === '1';
    state.busy = true;
    els.pauseButton.disabled = true;
    let success = false;
    try {
      await runCommand([active ? 'pause' : 'resume']);
      state.dirty = false;
      success = true;
      toast(active ? '守护已暂停，系统将接管参数' : '守护已恢复');
    } catch (err) {
      toast(err instanceof Error ? err.message : String(err), true);
    } finally {
      state.busy = false;
      els.pauseButton.disabled = false;
      if (state.kv) renderStatus(state.kv);
    }
    if (success) await refresh(true);
  }

  /* ---------- appearance ---------- */

  function applyAppearance() {
    const theme = document.body.dataset.theme;
    const wallpaper = document.body.dataset.wallpaper;
    const font = document.body.dataset.font;
    const meta = document.querySelector('meta[name="theme-color"]');
    if (meta) meta.setAttribute('content', THEME_COLORS[theme] || THEME_COLORS.lily);

    $$('.theme-option').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.theme === theme)));
    $$('.wall-option').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.wallpaper === wallpaper)));
    $$('#fontGrid button').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.font === font)));

    const custom = safeGet(LS.customWall);
    els.customWall.style.backgroundImage = custom ? `url("${custom}")` : '';
    if (wallpaper === 'custom' && !custom) {
      document.body.dataset.wallpaper = 'garden';
      safeSet(LS.wallpaper, 'garden');
      $$('.wall-option').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.wallpaper === 'garden')));
    }
  }

  function chooseTheme(theme) {
    if (!THEME_COLORS[theme]) return;
    document.body.dataset.theme = theme;
    safeSet(LS.theme, theme);
    applyAppearance();
  }

  function chooseWallpaper(wallpaper) {
    const valid = ['garden', 'water', 'linen', 'plain', 'custom'];
    if (!valid.includes(wallpaper)) return;
    if (wallpaper === 'custom' && !safeGet(LS.customWall)) {
      els.upload.click();
      return;
    }
    document.body.dataset.wallpaper = wallpaper;
    safeSet(LS.wallpaper, wallpaper);
    applyAppearance();
  }

  function chooseFont(font) {
    if (!['serif', 'song', 'sans', 'mono'].includes(font)) return;
    document.body.dataset.font = font;
    safeSet(LS.font, font);
    applyAppearance();
  }

  function setSync(enabled) {
    state.sync = Boolean(enabled);
    els.syncToggle.setAttribute('aria-pressed', String(state.sync));
    safeSet(LS.sync, state.sync ? '1' : '0');
  }

  function handleWallpaperFile(file) {
    if (!file) return;
    if (!file.type || !file.type.startsWith('image/')) {
      toast('请选择图片文件', true);
      return;
    }
    if (file.size > 14 * 1024 * 1024) {
      toast('图片超过 14MB，请先压缩', true);
      return;
    }

    const reader = new FileReader();
    reader.onload = () => {
      const img = new Image();
      img.onload = () => {
        const maxW = 1500;
        const maxH = 2200;
        const scale = Math.min(1, maxW / img.width, maxH / img.height);
        const canvas = document.createElement('canvas');
        canvas.width = Math.max(1, Math.round(img.width * scale));
        canvas.height = Math.max(1, Math.round(img.height * scale));
        const ctx = canvas.getContext('2d');
        if (!ctx) {
          toast('无法处理这张图片', true);
          return;
        }
        ctx.fillStyle = '#f2efe7';
        ctx.fillRect(0, 0, canvas.width, canvas.height);
        ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
        let data = canvas.toDataURL('image/jpeg', 0.82);
        if (data.length > 3500000) data = canvas.toDataURL('image/jpeg', 0.7);
        if (data.length > 3900000) {
          toast('压缩后仍太大，请换一张图片', true);
          return;
        }
        if (!safeSet(LS.customWall, data)) return;
        els.customWall.style.backgroundImage = `url("${data}")`;
        document.body.dataset.wallpaper = 'custom';
        safeSet(LS.wallpaper, 'custom');
        applyAppearance();
        toast('壁纸已更新');
        els.upload.value = '';
      };
      img.onerror = () => toast('无法读取这张图片', true);
      img.src = String(reader.result);
    };
    reader.onerror = () => toast('无法读取文件', true);
    reader.readAsDataURL(file);
  }

  function clearCustomWallpaper() {
    try { window.localStorage.removeItem(LS.customWall); } catch (err) { /* ignore */ }
    els.customWall.style.backgroundImage = '';
    if (document.body.dataset.wallpaper === 'custom') {
      document.body.dataset.wallpaper = 'garden';
      safeSet(LS.wallpaper, 'garden');
    }
    applyAppearance();
    toast('已移除自定义壁纸');
  }

  function resetLook() {
    try {
      window.localStorage.removeItem(LS.theme);
      window.localStorage.removeItem(LS.wallpaper);
      window.localStorage.removeItem(LS.font);
      window.localStorage.removeItem(LS.customWall);
    } catch (err) { /* ignore */ }
    document.body.dataset.theme = DEFAULTS.theme;
    document.body.dataset.wallpaper = DEFAULTS.wallpaper;
    document.body.dataset.font = DEFAULTS.font;
    els.customWall.style.backgroundImage = '';
    applyAppearance();
    toast('已恢复默认外观');
  }

  function loadAppearance() {
    const theme = safeGet(LS.theme);
    const wallpaper = safeGet(LS.wallpaper);
    const font = safeGet(LS.font);

    if (theme && THEME_COLORS[theme]) document.body.dataset.theme = theme;
    if (font && ['serif', 'song', 'sans', 'mono'].includes(font)) document.body.dataset.font = font;
    if (wallpaper && ['garden', 'water', 'linen', 'plain', 'custom'].includes(wallpaper)) {
      document.body.dataset.wallpaper = wallpaper;
    }
    state.sync = safeGet(LS.sync) === '1';
    applyAppearance();
  }

  /* ---------- sheet ---------- */

  function openSheet() {
    els.sheet.classList.add('is-open');
    els.sheet.setAttribute('aria-hidden', 'false');
    document.body.classList.add('is-locked');
    renderDiag();
    window.setTimeout(() => {
      const close = $('.sheet-head .icon-button', els.sheet);
      if (close) close.focus();
    }, 60);
  }

  function closeSheet() {
    els.sheet.classList.remove('is-open');
    els.sheet.setAttribute('aria-hidden', 'true');
    document.body.classList.remove('is-locked');
  }

  async function toggleLog() {
    if (!els.logBox.hidden) {
      els.logBox.hidden = true;
      return;
    }
    els.logBox.hidden = false;
    els.logBox.textContent = '读取中…';
    try {
      const text = await fetchLog();
      els.logBox.textContent = text.trim() || '暂无日志';
    } catch (err) {
      els.logBox.textContent = err instanceof Error ? err.message : String(err);
    }
  }

  /* ---------- events / boot ---------- */

  function bindEvents() {
    document.addEventListener('pointerdown', pressStart, { passive: true });
    document.addEventListener('pointerup', pressEnd, { passive: true });
    document.addEventListener('pointercancel', pressEnd, { passive: true });

    $('#settingsButton').addEventListener('click', openSheet);
    $$('[data-close="sheet"]').forEach((node) => node.addEventListener('click', closeSheet));
    els.applyButton.addEventListener('click', applyValues);
    els.pauseButton.addEventListener('click', togglePause);
    els.syncToggle.addEventListener('click', () => {
      setSync(!state.sync);
      toast(state.sync ? '同步三簇已开启' : '同步三簇已关闭');
    });

    els.themeGrid.addEventListener('click', (event) => {
      const button = event.target.closest('.theme-option');
      if (button) chooseTheme(button.dataset.theme);
    });
    els.wallpaperGrid.addEventListener('click', (event) => {
      const button = event.target.closest('.wall-option');
      if (button) chooseWallpaper(button.dataset.wallpaper);
    });
    els.fontGrid.addEventListener('click', (event) => {
      const button = event.target.closest('button[data-font]');
      if (button) chooseFont(button.dataset.font);
    });
    els.sceneReload.addEventListener('click', () => loadScenePresets(true));

    els.upload.addEventListener('change', () => handleWallpaperFile(els.upload.files && els.upload.files[0]));
    $('#clearWallpaper').addEventListener('click', clearCustomWallpaper);
    $('#resetLook').addEventListener('click', resetLook);
    $('#logToggle').addEventListener('click', toggleLog);

    document.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' && els.sheet.classList.contains('is-open')) closeSheet();
    });

    document.addEventListener('visibilitychange', () => {
      if (!document.hidden) refresh(true);
    });
  }

  function init() {
    loadAppearance();
    setSync(state.sync);
    bindEvents();

    state.demo = !hasKsu();
    els.demoBanner.hidden = !state.demo;

    if (state.demo && new URLSearchParams(window.location.search).has('reset')) {
      try { window.localStorage.removeItem(LS.demo); } catch (err) { /* ignore */ }
    }

    refresh();
    loadScenePresets(false);
    state.refreshTimer = window.setInterval(() => {
      if (!document.hidden && !state.busy) refresh(true);
    }, 4000);

    if (new URLSearchParams(window.location.search).has('settings')) {
      window.setTimeout(openSheet, 250);
    }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
