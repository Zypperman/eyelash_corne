/*
 * Asserts that the IDE rebuilds and re-renders the dongle entirely in the browser:
 * the edit is configured (Pyodide), compiled and linked (YoWASP clang/wasm-ld in a Web
 * Worker) and run (WebAssembly in the page) with the network switched off, and the dongle's
 * pixels change to match the edit.
 *
 *   python3 oled-emulator/lvgl-sim/build.py --ide --vendor <node_modules>
 *   (cd oled-emulator && python3 -m http.server 8765) &
 *   node oled-emulator/test/ide-dongle-runtime.test.js [http://localhost:8765/]
 *
 * Needs Playwright with Chromium (require('playwright')).
 */
const assert = require('node:assert/strict');
const { chromium } = require('playwright');

const SITE = process.argv[2] || 'http://localhost:8765/';
const BATTERY = 'dongle-display/boards/shields/dongle_display/widgets/battery_status.c';

async function main() {
  const browser = await chromium.launch();
  const context = await browser.newContext({ viewport: { width: 1300, height: 1000 } });
  const page = await context.newPage();
  const origin = new URL(SITE).origin;
  const foreign = [];
  const offlineRequests = [];
  let offline = false;
  context.on('request', (r) => {
    if (!r.url().startsWith(origin) && !r.url().startsWith('data:') && !r.url().startsWith('blob:')) foreign.push(r.url());
    if (offline) offlineRequests.push(r.url());
  });
  const workers = [];
  page.on('worker', (w) => workers.push(w.url()));
  page.on('pageerror', (e) => console.log('[pageerror]', e.message));
  // record the modules the page compiles itself
  await context.addInitScript(() => {
    window.__compiled = [];
    const compile = WebAssembly.compile;
    WebAssembly.compile = function (bytes) {
      window.__compiled.push(bytes.byteLength);
      return compile.apply(this, arguments);
    };
  });

  await page.goto(SITE + 'index.html');
  await page.waitForFunction(() => document.querySelector('#buildStatus') && /recompiled|started/.test(document.querySelector('#buildStatus').textContent));
  await page.click('button[data-tab=ide]');
  await page.waitForSelector('#tab-ide .ide-tree button');
  await page.click('#tab-ide .seg button[data-v=dongle]');

  const edit = async (path, from, to) => {
    await page.click(`#tab-ide .ide-tree button[data-path="${path}"]`);
    await page.evaluate(([from, to]) => {
      const cm = document.querySelector('#tab-ide .CodeMirror').CodeMirror;
      if (!cm.getValue().includes(from)) throw new Error('edit anchor not found: ' + from);
      cm.setValue(cm.getValue().replace(from, to));
    }, [from, to]);
  };
  const build = async () => {
    await page.click('#tab-ide .btn.primary');
    await page.waitForFunction(() => /Running on|failed/.test(document.querySelector('#tab-ide .ide-log').innerText), null, { timeout: 300000 });
    return page.$eval('#tab-ide .ide-log', (e) => e.innerText);
  };
  const pixels = () => page.evaluate(() => {
    const c = document.getElementById('cv-dongle');
    return Array.from(c.getContext('2d').getImageData(0, 0, c.width, c.height).data.filter((_, i) => i % 4 === 0)).join(',');
  });

  // 1. online warm-up: loads Pyodide and both YoWASP tools (clang for the compile, wasm-ld for the link)
  await edit(BATTERY, '#include "battery_status.h"', '#include "battery_status.h"\n/* warm-up */');
  const warm = await build();
  assert.match(warm, /Running on the dongle/, 'warm-up build failed:\n' + warm);

  // 2. offline: a visible edit (the battery labels gain a suffix), configured, compiled, linked and run
  const before = await pixels();
  const compiledBefore = await page.evaluate(() => window.__compiled.length);
  await context.setOffline(true);
  offline = true;
  await edit(BATTERY, '"%4u%%"', '"%3u%%!"');
  const log = await build();
  offline = false;
  await context.setOffline(false);

  assert.match(log, /Configuring Dongle/, 'Kconfig did not run:\n' + log);
  assert.match(log, new RegExp('✓ ' + BATTERY.replace(/[.]/g, '\\.') + ' \\(edited\\)'), 'the edited widget was not recompiled:\n' + log);
  assert.match(log, /Running on the dongle/, 'offline build did not run:\n' + log);
  const served = offlineRequests.filter((u) => !u.startsWith('blob:') && !u.startsWith('data:'));
  assert.deepEqual(served, [], 'the offline build fetched from the network');
  assert.ok(workers.some((u) => u.endsWith('/ide-worker.js')), 'compiler did not run in a Web Worker: ' + workers.join(', '));
  const compiled = await page.evaluate(() => window.__compiled);
  assert.ok(compiled.length > compiledBefore, 'the page did not compile the new module with WebAssembly.compile');
  assert.match(await page.$eval('#buildStatus', (e) => e.textContent), /IDE build/);

  await page.waitForTimeout(500);
  const after = await pixels();
  assert.notEqual(after, before, 'the dongle did not re-render with the edit');
  // the page's only other host is Google Fonts, for its own type
  assert.deepEqual(foreign.filter((u) => !/^https:\/\/fonts\.(googleapis|gstatic)\.com\//.test(u)), [], 'requests left the site');

  console.log('ok: the dongle edit was configured, compiled and linked in a Worker and run with WebAssembly, offline');
  console.log(`   ${compiled.length - compiledBefore} module(s) compiled in the page; worker: ${workers.find((u) => u.endsWith('/ide-worker.js'))}`);
  await browser.close();
}

main().catch((err) => { console.error('FAIL:', err.message); process.exit(1); });
