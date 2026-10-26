/*
 * The IDE tab: edit the firmware's display sources and .conf files, recompile in the browser,
 * and run the result on the simulated display.
 *
 * Configure: simconfig.py (the same module build.py uses) runs in Pyodide over the edited
 *   tree, evaluating the real Kconfig and the display module's CMakeLists.txt.
 * Compile: YoWASP clang in ide-worker.js recompiles only sources whose text, included files,
 *   or referenced CONFIG_ definitions changed from the prebuilt objects in ide/.
 * Link + run: wasm-ld links them with the prebuilt objects, and the page swaps the module in.
 */
const BASE = new URL('.', import.meta.url).href;
const VENDOR = BASE + 'vendor/';
const EDITS_KEY = 'oledsim-ide-edits';

const DISPLAYS = {
  left: { label: 'Left half' },
  right: { label: 'Right half' },
  dongle: { label: 'Dongle' },
};

function el(tag, { dataset, ...props } = {}, ...children) {
  const e = Object.assign(document.createElement(tag), props);
  if (dataset) Object.assign(e.dataset, dataset);
  for (const c of children) if (c != null) e.append(c);
  return e;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ide/ and vendor/ are generated (build.py --ide --vendor), so a site without them answers with its
// HTML 404 or fallback page; say so instead of JSON.parse's "Unexpected token '<'"
const BUNDLE_HINT = 'This site was published without the IDE bundle: build it with ' +
  '`python3 oled-emulator/lvgl-sim/build.py --ide --vendor <node_modules>` (see .github/workflows/pages.yml) or use the GitHub Pages site.';
async function fetchJson(path) {
  let r;
  try { r = await fetch(BASE + path); } catch (err) { throw new Error(`${path}: ${err.message}. ${BUNDLE_HINT}`); }
  if (!r.ok) throw new Error(`${path}: HTTP ${r.status}. ${BUNDLE_HINT}`);
  const text = (await r.text()).replace(/^\uFEFF/, '');
  try {
    return JSON.parse(text);
  } catch (err) {
    const start = text.trimStart().slice(0, 24);
    throw new Error(`${path} is not JSON (it starts ${JSON.stringify(start)}${/^</.test(start) ? ', an HTML page' : ''}). ${BUNDLE_HINT}`);
  }
}

/* FNV-1a, enough to tell file versions apart; texts repeat a lot, so remember them */
const hashes = new Map();
function hash(str) {
  let v = hashes.get(str);
  if (v === undefined) {
    let h = 0x811c9dc5;
    for (let i = 0; i < str.length; i++) { h ^= str.charCodeAt(i); h = Math.imul(h, 0x01000193); }
    v = (h >>> 0).toString(36);
    if (hashes.size > 20000) hashes.clear();
    hashes.set(str, v);
  }
  return v;
}

class Worker2 {
  constructor() {
    this.w = new Worker(new URL('ide-worker.js', import.meta.url), { type: 'module' });
    this.n = 0;
    this.pending = new Map();
    this.w.onmessage = ({ data }) => { const p = this.pending.get(data.id); this.pending.delete(data.id); p(data); };
    this.w.onerror = (e) => { for (const p of this.pending.values()) p({ error: e.message || 'worker failed' }); this.pending.clear(); };
  }
  call(msg, transfer = []) {
    const id = ++this.n;
    return new Promise((res) => { this.pending.set(id, res); this.w.postMessage({ ...msg, id }, transfer); });
  }
}

export async function mountIde(host, sim) {
  host.innerHTML = '';
  const ui = buildUi(host);
  const ide = new Ide(sim, ui);
  await ide.start();
  return ide;
}

function buildUi(host) {
  const ui = {};
  ui.seg = el('div', { className: 'seg' });
  for (const [id, d] of Object.entries(DISPLAYS)) ui.seg.append(el('button', { type: 'button', textContent: d.label, dataset: { v: id } }));
  ui.build = el('button', { className: 'btn primary', type: 'button', textContent: 'Build & run', title: 'Ctrl/Cmd+S' });
  ui.revert = el('button', { className: 'btn', type: 'button', textContent: 'Use firmware build' });
  ui.status = el('span', { className: 'note', style: 'margin:0' });
  ui.progress = el('div', { className: 'progress' }, el('i'));
  ui.toolbar = el('div', { className: 'card', style: 'display:grid;gap:8px' },
    el('div', { className: 'row' }, ui.seg, ui.build, ui.revert, ui.status), ui.progress,
    el('p', { className: 'note', style: 'margin:0', innerHTML: 'Edit any file below, then build. The configuration (Kconfig, CMakeLists.txt, devicetree) is re-evaluated, only what changed is recompiled, and the result replaces that display above. Edits stay in this browser until you reset them.' }));
  ui.tree = el('div', { className: 'ide-tree', role: 'tree' });
  ui.path = el('code');
  ui.fileReset = el('button', { className: 'btn', type: 'button', textContent: 'Reset file' });
  ui.download = el('button', { className: 'btn', type: 'button', textContent: 'Download' });
  ui.changes = el('span', { className: 'note', style: 'margin:0' });
  ui.editorBox = el('div', { className: 'ide-editor' });
  ui.log = el('div', { className: 'ide-log', textContent: 'Starting…' });
  host.append(ui.toolbar, el('div', { className: 'ide' }, ui.tree,
    el('div', { className: 'ide-main' }, el('div', { className: 'row' }, ui.path, ui.fileReset, ui.download, ui.changes), ui.editorBox, ui.log)));
  return ui;
}

class Ide {
  constructor(sim, ui) {
    this.sim = sim;
    this.ui = ui;
    this.display = 'left';
    this.files = null;      // bundle text, path -> string
    this.edits = new Map(); // path -> edited text
    this.objCache = new Map();
    this.prebuilt = {};     // base build -> ArrayBuffer
    this.memo = { incs: new Map(), refs: new Map() };
    this.busy = false;
  }

  log(text, cls) {
    const line = el('div', { className: cls || '' });
    // make "path:line:col:" clickable
    const parts = String(text).split(/((?:[\w.+-]+\/)+[\w.+-]+\.(?:c|h|conf|txt|dtsi|overlay|keymap|defconfig|Kconfig\w*):\d+(?::\d+)?)/);
    for (const part of parts) {
      const m = part.match(/^(.+?):(\d+)(?::\d+)?$/);
      if (m && this.files && (this.files[m[1]] !== undefined)) {
        const a = el('a', { textContent: part });
        a.addEventListener('click', () => this.open(m[1], +m[2]));
        line.append(a);
      } else line.append(part);
    }
    this.ui.log.append(line);
    this.ui.log.scrollTop = this.ui.log.scrollHeight;
  }
  setStatus(text) { this.ui.status.textContent = text; }
  setProgress(f) { this.ui.progress.firstChild.style.width = Math.round(f * 100) + '%'; }

  async start() {
    this.ui.log.textContent = '';
    this.log('Loading sources…');
    // vendor/versions.json first: Pyodide and clang load from vendor/ and fail far less clearly
    const [manifest, files] = await Promise.all([
      fetchJson('vendor/versions.json').then(() => fetchJson('ide/manifest.json')),
      fetchJson('ide/files.json'),
    ]);
    this.manifest = manifest;
    this.files = files;
    try { this.edits = new Map(Object.entries(JSON.parse(localStorage.getItem(EDITS_KEY) || '{}'))); } catch (err) { /* private mode */ }
    for (const p of [...this.edits.keys()]) if (!(p in files)) this.edits.delete(p);
    this.log(`${Object.keys(files).length} files from the firmware build (zmk-nice-oled ${manifest.shas.nice_oled.slice(0, 7)}, zmk-dongle-display ${manifest.shas.dongle_display.slice(0, 7)}).`);
    await this.loadEditor();
    this.bindUi();
    this.selectDisplay('left');
    this.log('Ready. The compiler (about 30 MB) and Python load on the first build.');
  }

  /* ---------------- editor ---------------- */
  async loadEditor() {
    const load = (src) => new Promise((res, rej) => { const s = el('script', { src, onload: res, onerror: rej }); document.head.append(s); });
    try {
      document.head.append(el('link', { rel: 'stylesheet', href: VENDOR + 'codemirror/codemirror.css' }));
      await load(VENDOR + 'codemirror/codemirror.js');
      await Promise.all(['clike', 'properties', 'python', 'yaml', 'cmake'].map((m) => load(`${VENDOR}codemirror/mode/${m}.js`)));
      document.head.append(el('style', { textContent: `
        .ide-editor .CodeMirror { height: 60vh; min-height: 360px; font: 13px/1.5 var(--font-mono); background: var(--field); color: var(--fg); }
        .ide-editor .CodeMirror-gutters { background: var(--panel); border-right: 1px solid var(--line); }
        .ide-editor .CodeMirror-linenumber { color: var(--muted); }
        .ide-editor .CodeMirror-cursor { border-left-color: var(--fg); }
        .ide-editor .CodeMirror-selected { background: rgba(60, 179, 113, .25) !important; }
        .ide-editor .cm-keyword, .ide-editor .cm-meta { color: #c678dd; } .ide-editor .cm-string { color: #98c379; }
        .ide-editor .cm-number { color: #d19a66; } .ide-editor .cm-comment { color: var(--muted); font-style: italic; }
        .ide-editor .cm-def, .ide-editor .cm-variable-2 { color: #61afef; } .ide-editor .cm-type, .ide-editor .cm-variable-3 { color: #e5c07b; }
        .ide-editor .cm-error-line { background: rgba(239, 111, 94, .18); }` }));
      this.cm = window.CodeMirror(this.ui.editorBox, { lineNumbers: true, indentUnit: 4, tabSize: 4, lineWrapping: false });
      this.cm.on('change', () => { if (!this.loading) this.onEdit(this.cm.getValue()); });
      this.cm.setOption('extraKeys', { 'Ctrl-S': () => this.build(), 'Cmd-S': () => this.build() });
    } catch (err) {
      this.ta = el('textarea', { spellcheck: false });
      this.ta.addEventListener('input', () => this.onEdit(this.ta.value));
      this.ui.editorBox.append(this.ta);
    }
  }
  setEditorText(text, mode) {
    this.loading = true;
    if (this.cm) { this.cm.setOption('mode', mode); this.cm.setValue(text); this.cm.clearHistory(); }
    else this.ta.value = text;
    this.loading = false;
  }
  text(path) { return this.edits.has(path) ? this.edits.get(path) : this.files[path]; }
  edited(path) { return this.edits.has(path) && this.edits.get(path) !== this.files[path]; }

  open(path, line) {
    this.path = path;
    const mode = /\.(c|h)$/.test(path) ? 'text/x-csrc' : /\.py$/.test(path) ? 'python' : /\.ya?ml$/.test(path) ? 'yaml'
      : /CMakeLists\.txt$/.test(path) ? 'cmake' : 'text/x-properties';
    this.setEditorText(this.text(path), mode);
    this.ui.path.textContent = path;
    for (const b of this.ui.tree.querySelectorAll('button')) b.classList.toggle('on', b.dataset.path === path);
    if (line && this.cm) {
      this.cm.focus();
      this.cm.setCursor({ line: line - 1, ch: 0 });
      this.cm.scrollIntoView({ line: line - 1, ch: 0 }, 120);
      const handle = this.cm.addLineClass(line - 1, 'background', 'cm-error-line');
      setTimeout(() => this.cm.removeLineClass(handle, 'background', 'cm-error-line'), 4000);
    }
  }
  onEdit(text) {
    if (!this.path) return;
    if (text === this.files[this.path]) this.edits.delete(this.path); else this.edits.set(this.path, text);
    this.saveEdits();
    this.markTree();
  }
  saveEdits() {
    clearTimeout(this.saveTimer);
    this.saveTimer = setTimeout(() => {
      try { localStorage.setItem(EDITS_KEY, JSON.stringify(Object.fromEntries(this.edits))); }
      catch (err) { this.log('Could not save edits in this browser (storage full or blocked); they last until you leave the page.', 'warn'); }
    }, 400);
    const n = [...this.edits.keys()].filter((p) => this.edited(p)).length;
    this.ui.changes.textContent = n ? `${n} changed file${n > 1 ? 's' : ''}` : '';
  }

  /* ---------------- file tree ---------------- */
  // the firmware build behind the chosen display, and the prebuilt objects it starts from
  target() {
    if (this.display === 'dongle') return { target: 'dongle', base: 'dongle' };
    if (this.display === 'left' && this.sim.role('left') === 'central') return { target: 'central', base: 'central' };
    return { target: this.display, base: 'peripheral' };
  }
  renderTree() {
    const { target, base } = this.target();
    const paths = Object.keys(this.files);
    const module = target === 'dongle' ? 'dongle-display/boards/shields/dongle_display' : 'nice-oled/boards/shields/nice_oled';
    const groups = [
      ['Your config', paths.filter((p) => p.startsWith('repo/') || p.startsWith('board-eyelash/') || (p.startsWith(module + '/') && /\.(conf|overlay)$|Kconfig|CMakeLists/.test(p))), true],
      [module.split('/')[0], paths.filter((p) => p.startsWith(module + '/') && !/\.(conf|overlay)$|Kconfig|CMakeLists/.test(p)), true],
      ['Simulator (ZMK + Zephyr stand-ins)', paths.filter((p) => p.startsWith('sim/')), false],
      ['LVGL 8.3.7', paths.filter((p) => p.startsWith('lvgl/')), false],
      ['Kconfig sources', paths.filter((p) => p.startsWith('zmk/') || p.startsWith('zephyr/')), false],
    ];
    this.ui.tree.innerHTML = '';
    for (const [name, list, open] of groups) {
      const d = el('details', { open }, el('summary', { textContent: `${name} (${list.length})` }));
      for (const p of list.sort()) {
        const short = p.startsWith(module + '/') ? p.slice(module.length + 1) : p.replace(/^repo\//, '');
        const b = el('button', { type: 'button', textContent: short, title: p, dataset: { path: p } });
        b.addEventListener('click', () => this.open(p));
        d.append(b);
      }
      this.ui.tree.append(d);
    }
    this.markTree();
    this.setStatus(`${DISPLAYS[this.display].label}: ${target === 'central' ? 'left half as central (preview)' : target} build`);
  }
  markTree() {
    for (const b of this.ui.tree.querySelectorAll('button')) b.classList.toggle('edited', this.edited(b.dataset.path));
  }
  selectDisplay(id) {
    this.display = id;
    for (const b of this.ui.seg.querySelectorAll('button')) b.classList.toggle('on', b.dataset.v === id);
    this.renderTree();
    if (!this.path) {
      const first = id === 'dongle' ? 'dongle-display/boards/shields/dongle_display/custom_status_screen.c' : 'repo/config/eyelash_corne.conf';
      this.open(first in this.files ? first : Object.keys(this.files)[0]);
    }
  }
  bindUi() {
    this.ui.seg.addEventListener('click', (e) => { const b = e.target.closest('button'); if (b) this.selectDisplay(b.dataset.v); });
    this.ui.build.addEventListener('click', () => this.build());
    this.ui.revert.addEventListener('click', async () => { await this.sim.revertBuild(this.display); this.log(`${DISPLAYS[this.display].label} is back on the firmware build.`); });
    this.ui.fileReset.addEventListener('click', () => { if (!this.path) return; this.edits.delete(this.path); this.saveEdits(); this.open(this.path); this.markTree(); });
    this.ui.download.addEventListener('click', () => {
      if (!this.path) return;
      const a = el('a', { href: URL.createObjectURL(new Blob([this.text(this.path)], { type: 'text/plain' })), download: this.path.split('/').pop() });
      document.body.append(a); a.click(); a.remove();
    });
    window.addEventListener('oledsim:role', () => this.renderTree());
    document.addEventListener('keydown', (e) => {
      if ((e.ctrlKey || e.metaKey) && e.key === 's' && document.querySelector('#tab-ide.on')) { e.preventDefault(); this.build(); }
    });
    this.saveEdits();
  }

  /* ---------------- toolchain ---------------- */
  async ensurePython() {
    if (this.py) return this.py;
    this.log('Starting Python (Pyodide) for Kconfig…');
    const { loadPyodide } = await import(VENDOR + 'pyodide/pyodide.mjs');
    const py = await loadPyodide({ indexURL: VENDOR + 'pyodide/' });
    const FS = py.FS;
    // /root holds the edited tree; /pristine the firmware's, to tell what an edit changed
    for (const [path, text] of Object.entries(this.files)) { this.pyWrite(FS, path, text); this.pyWrite(FS, path, text, '/pristine/'); }
    this.pySynced = new Map();
    this.pristine = {};
    py.runPython("import sys; sys.path[:0] = ['/root/py', '/root/sim']; import simconfig, json");
    this.py = py;
    return py;
  }
  pyWrite(FS, path, text, root = '/root/') {
    const full = root + path;
    FS.mkdirTree(full.slice(0, full.lastIndexOf('/')));
    FS.writeFile(full, text);
  }
  syncPython() {
    const FS = this.py.FS;
    const want = new Map([...this.edits].filter(([p]) => this.edited(p)));
    for (const [p, t] of want) if (this.pySynced.get(p) !== t) this.pyWrite(FS, p, t);
    for (const p of this.pySynced.keys()) if (!want.has(p)) this.pyWrite(FS, p, this.files[p]);
    this.pySynced = new Map(want);
    // simconfig is imported once; reload it if it was edited
    if (this.pySynced.has('sim/simconfig.py') || this.simconfigDirty) {
      this.py.runPython('import importlib; importlib.reload(simconfig)');
      this.simconfigDirty = this.pySynced.has('sim/simconfig.py');
    }
  }
  async ensureWorker() {
    if (this.worker) return this.worker;
    this.log('Loading the compiler (YoWASP clang, ~30 MB on first use)…');
    const w = new Worker2();
    const files = {};
    for (const [p, t] of Object.entries(this.files)) if (/\.(c|h|inc)$/.test(p)) files[p] = t;
    const r = await w.call({ type: 'init', clangUrl: VENDOR + 'yowasp-clang/bundle.js', files });
    if (r.error) throw new Error(r.error);
    this.worker = w;
    this.workerSynced = new Map();
    return w;
  }
  async syncWorker(generated) {
    const want = new Map();
    for (const [p, t] of this.edits) if (this.edited(p) && /\.(c|h|inc)$/.test(p)) want.set(p, t);
    for (const [p, t] of Object.entries(generated)) want.set(p, t);
    const update = {};
    for (const [p, t] of want) if (this.workerSynced.get(p) !== t) update[p] = t;
    for (const p of this.workerSynced.keys()) if (!want.has(p) && p in this.files) update[p] = this.files[p];
    if (Object.keys(update).length) await this.worker.call({ type: 'update', files: update });
    this.workerSynced = want;
  }
  async prebuiltObjects(base) {
    if (!this.prebuilt[base]) {
      this.prebuilt[base] = fetch(`${BASE}ide/${base}.objs`).then((r) => { if (!r.ok) throw new Error(`ide/${base}.objs is missing`); return r.arrayBuffer(); });
    }
    return this.prebuilt[base];
  }

  /* ---------------- dependency tracking ---------------- */
  // direct includes of a file, resolved like the compiler would against `includes`
  includesOf(path, includes, lookup) {
    const text = lookup(path);
    const memoKey = path + '\u0000' + includes.join(',') + '\u0000' + hash(text);
    if (this.memo.incs.has(memoKey)) return this.memo.incs.get(memoKey);
    const out = [];
    const dir = path.slice(0, path.lastIndexOf('/'));
    const norm = (p) => { const o = []; for (const s of p.split('/')) { if (s === '..') o.pop(); else if (s && s !== '.') o.push(s); } return o.join('/'); };
    for (const m of text.matchAll(/^\s*#\s*include\s*([<"])([^>"]+)[>"]/gm)) {
      const name = m[2], cands = [];
      if (m[1] === '"') cands.push(norm(dir + '/' + name));
      for (const inc of includes) cands.push(norm(inc + '/' + name));
      cands.push(norm(name));
      const hit = cands.find((c) => lookup(c) !== undefined);
      if (hit) out.push(hit);
    }
    this.memo.incs.set(memoKey, out);
    return out;
  }
  closure(src, includes, lookup) {
    const seen = new Set([src]), stack = [src];
    while (stack.length) {
      for (const dep of this.includesOf(stack.pop(), includes, lookup)) if (!seen.has(dep)) { seen.add(dep); stack.push(dep); }
    }
    return seen;
  }
  refsOf(text) {
    const h = hash(text);
    if (!this.memo.refs.has(h)) this.memo.refs.set(h, new Set(text.match(/\bCONFIG_\w+/g) || []));
    return this.memo.refs.get(h);
  }
  static defines(autoconf) {
    const out = new Map();
    for (const m of (autoconf || '').matchAll(/^#define (CONFIG_\w+) (.*)$/gm)) out.set(m[1], m[2]);
    return out;
  }

  // what the edits did to the configuration: changed symbols, and warnings the firmware's tree doesn't have
  reportConfig(target, plan) {
    if (!this.pristine[target]) {
      this.py.globals.set('sim_target', target);
      this.pristine[target] = JSON.parse(this.py.runPython(
        "json.dumps((lambda c: {'values': c['values'], 'warnings': c['warnings']})(simconfig.configure('/pristine', sim_target, workdir='/tmp/kconfig-pristine')))"));
    }
    const was = this.pristine[target].values, now = plan.values;
    const show = (v) => (v === undefined ? 'n' : v === 1 ? 'y' : JSON.stringify(v));
    const changed = [...new Set([...Object.keys(was), ...Object.keys(now)])].filter((k) => was[k] !== now[k]).sort();
    if (changed.length) {
      this.log(`Your edits change ${changed.length} symbol${changed.length > 1 ? 's' : ''}: ` +
        changed.slice(0, 16).map((k) => `CONFIG_${k} ${show(was[k])} → ${show(now[k])}`).join(', ') + (changed.length > 16 ? ', …' : ''));
    } else this.log('Your edits leave the configuration as it was.');
    const where = /^[^\s:]+:\d+: /;
    const known = new Set(this.pristine[target].warnings.map((w) => w.replace(where, '')));
    for (const w of plan.warnings) {
      if (known.has(w.replace(where, ''))) continue;
      this.log(w + (/undefined symbol/.test(w) ? ' (a typo, or a symbol outside the display code the simulator models)' : ''), 'warn');
    }
  }

  /* ---------------- build ---------------- */
  async build() {
    if (this.busy) return;
    this.busy = true;
    this.ui.build.disabled = true;
    this.ui.log.textContent = '';
    this.setProgress(0);
    const t0 = performance.now();
    try {
      const { target, base } = this.target();
      const baseline = this.manifest.builds[base];
      if (!baseline) throw new Error(`no prebuilt objects for ${base}`);

      // 1. configure
      const py = await this.ensurePython();
      this.syncPython();
      this.log(`Configuring ${DISPLAYS[this.display].label} (${target}): Kconfig, CMakeLists.txt, devicetree…`);
      py.globals.set('sim_target', target);
      py.globals.set('sim_gen', baseline.gen);
      let plan;
      try {
        plan = JSON.parse(py.runPython(`json.dumps(simconfig.plan('/root', sim_target, (), gen=sim_gen, workdir='/tmp/kconfig'))`));
      } catch (err) {
        const msg = String(err.message || err).split('\n').filter((l) => l.trim()).slice(-3).join('\n');
        throw new Error('configuration failed:\n' + msg);
      }
      const warnings = [];
      this.log(`${plan.sources.length} sources, panel ${plan.panel.width}×${plan.panel.height} ${plan.panel.controller}, ${plan.central ? 'central' : 'peripheral'}.`);
      if (this.edits.size) this.reportConfig(target, plan);

      // 2. what changed since the prebuilt objects
      const generated = plan.headers;
      const lookup = (p) => (p in generated ? generated[p] : this.text(p));
      const oldDefs = Ide.defines(baseline.headers[baseline.gen + '/autoconf.h']);
      const newDefs = Ide.defines(generated[plan.gen + '/autoconf.h']);
      const changedSyms = new Set();
      for (const k of new Set([...oldDefs.keys(), ...newDefs.keys()])) if (oldDefs.get(k) !== newDefs.get(k)) changedSyms.add(k);
      const changedGen = new Set(Object.keys(generated).filter((p) => !p.endsWith('/autoconf.h') && generated[p] !== baseline.headers[p]));
      const flagsChanged = JSON.stringify(plan.flags) !== JSON.stringify(baseline.flags);

      const work = [];
      for (const src of plan.sources) {
        if (lookup(src) === undefined) throw new Error(`${src}: no such file`);
        const deps = this.closure(src, plan.includes, lookup);
        let reason = null;
        if (!baseline.objects[src]) reason = 'new source';
        else if (flagsChanged) reason = 'flags';
        else if ([...deps].some((d) => this.edited(d))) reason = 'edited';
        else if ([...deps].some((d) => changedGen.has(d))) reason = 'generated header';
        else {
          for (const d of deps) { for (const s of this.refsOf(lookup(d))) if (changedSyms.has(s)) { reason = s; break; } if (reason) break; }
        }
        if (reason) {
          const key = src + '|' + hash(plan.flags.join(' ') + [...deps].map((d) => d + ':' + hash(lookup(d))).join(',') +
            [...deps].flatMap((d) => [...this.refsOf(lookup(d))]).map((s) => s + '=' + newDefs.get(s)).join(','));
          work.push({ src, reason, key });
        }
      }
      const compileList = work.filter((w) => !this.objCache.has(w.key));
      this.log(`${work.length} of ${plan.sources.length} sources differ from the firmware build; ${compileList.length} to compile${work.length > compileList.length ? `, ${work.length - compileList.length} cached` : ''}.`);

      // 3. compile
      const objects = new Map();
      if (compileList.length) {
        await this.ensureWorker();
        await this.syncWorker(generated);
        let done = 0, failed = 0;
        for (const w of compileList) {
          this.setStatus(`compiling ${w.src.split('/').pop()} (${done + 1}/${compileList.length})`);
          const r = await this.worker.call({ type: 'compile', path: w.src, flags: plan.flags });
          done++;
          this.setProgress(done / (compileList.length + 1));
          if (r.error) {
            failed++;
            this.log(`✗ ${w.src}`, 'err');
            for (const line of r.error.split('\n').filter(Boolean).slice(0, 40)) this.log('  ' + line);
            if (failed >= 3) break;
            continue;
          }
          this.objCache.set(w.key, r.object);
          this.log(`✓ ${w.src} (${w.reason})`);
          if (r.stderr && /error|warning/.test(r.stderr)) warnings.push(r.stderr);
        }
        if (failed) throw new Error(`${failed} file${failed > 1 ? 's' : ''} failed to compile; the display keeps running its last good build.`);
      }

      // 4. link: prebuilt objects for everything else
      this.setStatus('linking');
      const blob = await this.prebuiltObjects(base);
      const recompiled = new Map(work.map((w) => [w.src, this.objCache.get(w.key)]));
      const list = plan.sources.map((src) => {
        if (recompiled.has(src)) return recompiled.get(src);
        const [off, len] = baseline.objects[src];
        return new Uint8Array(blob, off, len).slice();
      });
      await this.ensureWorker();
      const r = await this.worker.call({ type: 'link', ldflags: plan.ldflags, objects: list });
      if (r.error) {
        for (const line of r.error.split('\n').filter(Boolean).slice(0, 30)) this.log('  ' + line);
        throw new Error('link failed; the display keeps running its last good build.');
      }
      this.setProgress(1);

      // 5. run
      const ok = await this.sim.runBuild(this.display, r.wasm, plan.meta);
      const secs = ((performance.now() - t0) / 1000).toFixed(1);
      if (ok) {
        this.log(`Running on the ${DISPLAYS[this.display].label.toLowerCase()} (${(r.wasm.length / 1024).toFixed(0)} KB, ${secs} s).`, 'ok');
        this.setStatus(`built in ${secs} s`);
      } else {
        this.log('The new build failed to start (see the error above the displays); the display keeps running its last good build.', 'err');
      }
    } catch (err) {
      this.log(String(err.message || err), 'err');
      this.setStatus('build failed');
    } finally {
      this.busy = false;
      this.ui.build.disabled = false;
    }
  }
}
