#!/usr/bin/env python3
"""Browser smoke test of NeoWEB's page against the host emulator.

    make -C web host && python3 web/tests/test_webui.py
    (NEOWEB_HOST_BIN=/path/to/neoweb-host picks another build)

Seeds the same fake console as test_api.py, then drives Chromium through
Playwright (node, `require('playwright')`): every section in every theme,
in both languages, at desktop and phone width and in lite mode, failing on
any script error or horizontal overflow; plus two upload checks (an upload
cancelled and resumed arrives whole; a smaller, different file already on
the console is not silently appended to). Skips when node or Playwright is
missing. External hosts (fonts, GitHub, ProsperoStore) may be unreachable:
the page must cope, so their failures are not errors here.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_api  # noqa: E402

SCRIPT = r"""
const { chromium } = require('playwright');
const fs = require('fs'), path = require('path'), crypto = require('crypto');
const [BASE, ROOT, TMP] = process.argv.slice(2);
const VIEWS = ["panel","library","stats","trophies","saves","captures","cheats","fixes","smp","game=PPSA01234",
  "game=CUSA00001","install","apps","homebrew","payloads","themes","remote","files","transfers","procs","system","settings"];
const RUNS = [["console","es",1400],["holo","es",1400],["oled","en",1400],["synth","en",1400],["holo","es",390],["console","en",390,1]];
const bad = [];
(async () => {
  const b = await chromium.launch();
  for (const [theme, lang, w, lite] of RUNS) {
    const p = await b.newPage({ viewport: { width: w, height: 900 } });
    p.on('pageerror', e => bad.push(`${theme}/${lang}/${w}: ${e.message}`));
    await p.addInitScript(c => localStorage.setItem('neoweb_ui', c), JSON.stringify({ theme, lang }));
    await p.goto(BASE + (lite ? '?lite=1' : ''), { waitUntil: 'domcontentloaded' });
    await p.waitForTimeout(1500);
    for (const v of VIEWS) {
      await p.evaluate(v => { location.hash = v }, v);
      await p.waitForTimeout(500);
      const ov = await p.evaluate(() => document.documentElement.scrollWidth - innerWidth);
      if (ov > 2) bad.push(`${theme}/${lang}/${w}: ${v} overflows by ${ov}px`);
    }
    await p.close();
  }
  // safe mode: said on the dashboard, and turned off from there
  {
    const safe = path.join(ROOT, 'data/neohen/safe_mode');
    const p = await b.newPage({ viewport: { width: 1400, height: 900 } });
    p.on('pageerror', e => bad.push('health: ' + e.message));
    await p.goto(BASE + '#panel', { waitUntil: 'domcontentloaded' });
    for (let t = 0; t < 5000 && !await p.$('#hl-safe'); t += 200) await p.waitForTimeout(200);
    if (!await p.$('#hl-safe')) bad.push('health: no safe mode notice');
    else {
      await p.evaluate(() => document.querySelector('[data-health="safe"]').click());
      for (let t = 0; t < 3000 && !await p.evaluate(() => document.getElementById('dlg').open); t += 100) await p.waitForTimeout(100);
      await p.click('#dyes');
      for (let t = 0; t < 3000 && fs.existsSync(safe); t += 100) await p.waitForTimeout(100);
      if (fs.existsSync(safe)) bad.push('health: safe mode not turned off');
    }
    await p.close();
  }
  // homebrew.js runs sandboxed: its menu shows, it cannot reach NeoWEB or this page
  {
    const p = await b.newPage({ viewport: { width: 1400, height: 900 } });
    p.on('pageerror', e => bad.push('homebrew: ' + e.message));
    await p.goto(BASE + '#homebrew', { waitUntil: 'domcontentloaded' });
    const btn = '[data-hb="open"][data-v="/data/homebrew/Probe"]';
    for (let t = 0; t < 5000 && !await p.$(btn); t += 200) await p.waitForTimeout(200);
    if (!await p.$(btn)) bad.push('homebrew: Probe is not listed');
    else {
      await p.click(btn);
      const txt = () => p.evaluate(() => (document.getElementById('hb-menu') || {}).textContent || '');
      for (let t = 0; t < 6000 && !/isolated|LEAK/.test(await txt()); t += 200) await p.waitForTimeout(200);
      const head = await txt();
      if (!/Probe menu/.test(head) || !/isolated/.test(head) || /LEAK/.test(head)) bad.push('homebrew: sandbox: ' + head);
      await p.evaluate(() => document.querySelector('#hb-menu [data-hbf]').click());
      for (let t = 0; t < 4000 && !/Level 1/.test(await txt()); t += 200) await p.waitForTimeout(200);
      if (!/Level 1/.test(await txt())) bad.push('homebrew: no carousel: ' + await txt());
    }
    await p.close();
  }
  // duplicates and leftovers in the library
  {
    const p = await b.newPage({ viewport: { width: 1400, height: 900 } });
    p.on('pageerror', e => bad.push('cleanup: ' + e.message));
    await p.goto(BASE + '#library', { waitUntil: 'domcontentloaded' });
    for (let t = 0; t < 5000 && !await p.$('#lib-cl'); t += 200) await p.waitForTimeout(200);
    await p.click('#lib-cl');
    const txt = () => p.evaluate(() => document.getElementById('clbox').textContent);
    for (let t = 0; t < 5000 && !/PPSA07777/.test(await txt()); t += 200) await p.waitForTimeout(200);
    if (!/backports\/PPSA07777/.test(await txt())) bad.push('cleanup: leftover not shown: ' + await txt());
    await p.close();
  }
  // the pad and the arrows: off on a PC until a pad is used; then the D-pad
  // moves the focus (with a ring), Cross presses, Circle closes or goes
  // back, R1/L1 change section; arrows in a field stay the field's
  {
    const p = await b.newPage({ viewport: { width: 1400, height: 900 } });
    p.on('pageerror', e => bad.push('pad: ' + e.message));
    await p.addInitScript(() => {
      window.__pad = { id: 'test pad', index: 0, connected: true, axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })) };
      window.__pads = [];
      navigator.getGamepads = () => window.__pads;
    });
    await p.goto(BASE + '#panel', { waitUntil: 'domcontentloaded' });
    await p.waitForTimeout(1200);
    const hash = () => p.evaluate(() => location.hash);
    const focus = () => p.evaluate(() => { const a = document.activeElement; return a === document.body ? '' : (a.getAttribute('data-v') || a.id || a.tagName) });
    await p.keyboard.press('ArrowDown');
    if (await focus()) bad.push('pad: the arrows move the focus on a PC with no pad');
    const tap = async i => {
      await p.evaluate(i => { __pad.buttons[i].pressed = true; if (!__pads.length) { __pads.push(__pad); dispatchEvent(new Event('gamepadconnected')) } }, i);
      await p.waitForTimeout(150);
      await p.evaluate(i => { __pad.buttons[i].pressed = false }, i);
      await p.waitForTimeout(150);
    };
    await tap(13);
    if (!await p.evaluate(() => document.documentElement.classList.contains('tv'))) bad.push('pad: not switched on by the pad');
    if (!await focus()) bad.push('pad: the D-pad focuses nothing');
    const ring = await p.evaluate(() => { const c = getComputedStyle(document.activeElement); return c.outlineStyle !== 'none' && parseFloat(c.outlineWidth) >= 2 });
    if (!ring) bad.push('pad: no focus ring');
    await tap(5);
    const h1 = await hash();
    await tap(5);
    const h2 = await hash();
    await tap(4);
    if (h1 === '#panel' || h2 === h1 || await hash() !== h1) bad.push(`pad: R1/L1 ${h1} ${h2} ${await hash()}`);
    await tap(1);
    if (await hash() !== '#panel') bad.push('pad: Circle does not go back to the dashboard: ' + await hash());
    await p.evaluate(() => { document.getElementById('dh').textContent = 'pad'; document.getElementById('dlg').showModal() });
    await tap(13);
    if (!await p.evaluate(() => !!document.activeElement.closest('dialog'))) bad.push('pad: the D-pad leaves an open dialog');
    await tap(1);
    if (await p.evaluate(() => document.getElementById('dlg').open)) bad.push('pad: Circle does not close the dialog');
    await p.evaluate(() => { location.hash = 'files' });
    await p.waitForTimeout(600);
    await p.focus('#f-q');
    await p.keyboard.type('ab');
    await p.keyboard.press('ArrowLeft');
    if (await p.evaluate(() => document.activeElement.id !== 'f-q' || document.activeElement.selectionStart !== 1)) bad.push('pad: the arrows leave a field being typed in');
    await tap(0);
    if (await p.evaluate(() => document.activeElement.id) !== 'f-q') bad.push('pad: Cross in a field moves away');
    await tap(1);
    if (await p.evaluate(() => document.activeElement.id) === 'f-q') bad.push('pad: Circle in a field does not leave it');
    await p.close();
  }
  // the app bits: the manifest is linked and parses, the theme colour follows
  // the theme, and on localhost (a secure context) the service worker answers
  // a page load with the "cannot be reached" page when the console is off
  {
    const ctx = await b.newContext();
    const p = await ctx.newPage();
    p.on('pageerror', e => bad.push('pwa: ' + e.message));
    await p.addInitScript(() => localStorage.setItem('neoweb_ui', JSON.stringify({ theme: 'oled' })));
    await p.goto(BASE, { waitUntil: 'load' });
    const cdp = await ctx.newCDPSession(p);
    const m = await cdp.send('Page.getAppManifest');
    if (!m.url.endsWith('/manifest.webmanifest') || (m.errors || []).length) bad.push('pwa: manifest ' + JSON.stringify(m.errors));
    if (await p.evaluate(() => document.getElementById('thc').content) !== '#000') bad.push('pwa: theme-color does not follow the theme');
    let ctl = false;
    for (let t = 0; t < 5000 && !(ctl = await p.evaluate(() => !!navigator.serviceWorker.controller)); t += 200) await p.waitForTimeout(200);
    if (!ctl) bad.push('pwa: no service worker on localhost');
    else {
      await ctx.setOffline(true);
      await p.reload().catch(() => {});
      if (!/cannot be reached/.test(await p.textContent('h1').catch(() => ''))) bad.push('pwa: no offline page');
      await ctx.setOffline(false);
      // and /api is never answered from it
      const r = await p.evaluate(() => fetch('/api/ping').then(r => r.status));
      if (r !== 200) bad.push('pwa: /api/ping ' + r);
    }
    await ctx.close();
  }
  // uploads
  const p = await b.newPage({ viewport: { width: 1400, height: 900 } });
  p.on('pageerror', e => bad.push('upload: ' + e.message));
  await p.goto(BASE + '#transfers', { waitUntil: 'domcontentloaded' });
  await p.waitForTimeout(1500);
  const sha = f => crypto.createHash('sha256').update(fs.readFileSync(f)).digest('hex');
  const row = () => p.evaluate(() => (document.querySelector('#v-transfers .item small') || {}).textContent || '');
  /* the list is redrawn as bytes go: press the button as it is now */
  const press = sel => p.evaluate(sel => document.querySelector(sel).click(), sel);
  const until = async (re, ms) => { for (let t = 0; t < ms; t += 200) { if (re.test(await row())) return true; await p.waitForTimeout(200); } return false; };
  const big = path.join(TMP, 'big.bin');
  fs.writeFileSync(big, crypto.randomBytes(40 * 1048576 + 7));
  const cdp = await p.context().newCDPSession(p);
  await cdp.send('Network.emulateNetworkConditions', { offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: 10 * 1048576 });
  await p.setInputFiles('#pick-f', big);
  await p.waitForTimeout(1500);
  await press('[data-uc]');
  if (!await until(/Cancel/, 3000)) bad.push('upload: not cancelled: ' + await row());
  await cdp.send('Network.emulateNetworkConditions', { offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: -1 });
  await press('[data-ur]');
  if (!await until(/Done|Completado/, 30000)) bad.push('upload: resume did not finish: ' + await row());
  else if (sha(big) !== sha(path.join(ROOT, 'data/big.bin'))) bad.push('upload: resumed file differs');
  if (!/resumed|reanudado/.test(await row())) bad.push('upload: was not resumed: ' + await row());
  await press('[data-a="clr"]');
  // a smaller, different file already there: asked, and replaced when told so
  const small = path.join(TMP, 'small.txt');
  fs.writeFileSync(small, crypto.randomBytes(1000));
  fs.writeFileSync(path.join(ROOT, 'data/small.txt'), Buffer.alloc(400));
  await p.setInputFiles('#pick-f', small);
  for (const k of ['#dno', '#dyes']) {
    for (let t = 0; t < 3000 && !await p.evaluate(() => document.getElementById('dlg').open); t += 100) await p.waitForTimeout(100);
    if (!await p.evaluate(() => document.getElementById('dlg').open)) { bad.push('upload: no question before ' + k); break; }
    await p.click(k);
  }
  if (!await until(/Done|Completado/, 5000)) bad.push('upload: replace did not finish: ' + await row());
  else if (sha(small) !== sha(path.join(ROOT, 'data/small.txt'))) bad.push('upload: the old file was appended to');
  await b.close();
  console.log(JSON.stringify(bad));
})().catch(e => { console.log(JSON.stringify(['crash: ' + e.message])); process.exit(0) });
"""


def node_env():
    env = dict(os.environ)
    try:
        root = subprocess.run(["npm", "root", "-g"], capture_output=True, text=True, timeout=30).stdout.strip()
        if root:
            env["NODE_PATH"] = root + os.pathsep + env.get("NODE_PATH", "")
    except (OSError, subprocess.SubprocessError):
        pass
    return env


class WebUiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("node"):
            raise unittest.SkipTest("node is not installed")
        cls.env = node_env()
        r = subprocess.run(["node", "-e", "require('playwright')"], env=cls.env, capture_output=True)
        if r.returncode:
            raise unittest.SkipTest("playwright is not installed")
        test_api.BIN = os.environ.get("NEOWEB_HOST_BIN", test_api.BIN)
        test_api.PayloadTest.setUpClass()
        cls.srv = test_api.PayloadTest
        test_api.seed_homebrew(cls.srv.root)
        probe = os.path.join(cls.srv.root, "data/homebrew/Probe")
        os.makedirs(probe)
        with open(os.path.join(probe, "homebrew.js"), "w") as f:
            f.write("""async function main(){
  let leak = '';
  try { parent.document.title; leak += 'LEAK-dom '; } catch (e) {}
  try { await fetch('http://127.0.0.1:%d/api/homebrew/list'); leak += 'LEAK-api '; } catch (e) {}
  return {mainText: 'Probe menu', secondaryText: leak || 'isolated',
          onclick: async () => { showCarousel([{mainText: 'Level 1', onclick: () => ({path: window.workingDir + '/x.elf'})}]); }};
}
""" % cls.srv.port)
        cls.tmp = tempfile.mkdtemp(prefix="neoweb-ui-")

    @classmethod
    def tearDownClass(cls):
        cls.srv.tearDownClass()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_sections_and_uploads(self):
        # NeoHEN in safe mode: every section is drawn with its notice on
        with open(os.path.join(self.srv.root, "data/neohen/safe_mode"), "w") as f:
            f.write("failed starts in a row: 3\n")
        js = os.path.join(self.tmp, "run.js")
        with open(js, "w") as f:
            f.write(SCRIPT)
        r = subprocess.run(["node", js, f"http://127.0.0.1:{self.srv.port}/", self.srv.root, self.tmp],
                           env=self.env, capture_output=True, text=True, timeout=600)
        lines = [l for l in r.stdout.splitlines() if l.startswith("[")]
        self.assertTrue(lines, r.stdout + r.stderr)
        self.assertEqual(json.loads(lines[-1]), [])

    def test_zz_access_and_neohen_settings(self):
        ini = os.path.join(self.srv.root, "data/neohen/config.ini")
        with open(ini, "w") as f:
            f.write(test_api.NEOHEN_INI)
        js = os.path.join(self.tmp, "access.js")
        with open(js, "w") as f:
            f.write(ACCESS_SCRIPT)
        r = subprocess.run(["node", js, f"http://127.0.0.1:{self.srv.port}/", self.srv.root],
                           env=self.env, capture_output=True, text=True, timeout=300)
        lines = [l for l in r.stdout.splitlines() if l.startswith("[")]
        self.assertTrue(lines, r.stdout + r.stderr)
        self.assertEqual(json.loads(lines[-1]), [])
        with open(ini) as f:
            text = f.read()
        self.assertIn("channel=prerelease\n", text)
        self.assertIn("apply_at_boot=on\n", text)
        self.assertIn("my_own_key=keep me\n", text)


# NeoHEN's settings, the token, the guest link and the QR, in the page
ACCESS_SCRIPT = r"""
const { chromium } = require('playwright');
const fs = require('fs'), path = require('path');
const [BASE, ROOT] = process.argv.slice(2);
const bad = [], cdn = [];
const cfgFile = path.join(ROOT, 'data/neohen/web/config.json');
const wait = async (p, f, ms) => { for (let t = 0; t < ms; t += 100) { if (await p.evaluate(f)) return true; await p.waitForTimeout(100); } return false; };
(async () => {
  const b = await chromium.launch();
  const ctx = await b.newContext({ viewport: { width: 390, height: 900 } });
  ctx.on('request', r => { if (!r.url().startsWith(BASE)) cdn.push(r.url()); });
  const p = await ctx.newPage();
  p.on('pageerror', e => bad.push('admin: ' + e.message));
  await p.addInitScript(() => localStorage.setItem('neoweb_ui', JSON.stringify({ lang: 'en' })));
  await p.goto(BASE + '#settings', { waitUntil: 'domcontentloaded' });
  await p.waitForTimeout(1200);
  await p.evaluate(() => document.querySelector('[data-ctab="neohen"]').click());
  if (!await wait(p, () => !!document.querySelector('select[data-nh="update.channel"]'), 4000)) bad.push('no NeoHEN settings');
  else {
    await p.selectOption('select[data-nh="update.channel"]', 'prerelease');
    await p.waitForTimeout(800);
    if (!/channel=prerelease/.test(fs.readFileSync(path.join(ROOT, 'data/neohen/config.ini'), 'utf8'))) bad.push('update.channel not saved');
    // the hand-off at boot to an installed update: off until asked
    if (await p.$eval('select[data-nh="update.apply_at_boot"]', e => e.value).catch(() => '') !== 'off') bad.push('update.apply_at_boot is not off');
    else {
      await p.selectOption('select[data-nh="update.apply_at_boot"]', 'on');
      await p.waitForTimeout(800);
      if (!/apply_at_boot=on/.test(fs.readFileSync(path.join(ROOT, 'data/neohen/config.ini'), 'utf8'))) bad.push('update.apply_at_boot not saved');
    }
  }
  await p.evaluate(() => document.querySelector('[data-ctab="all"]').click());
  await p.waitForTimeout(300);
  const ov = await p.evaluate(() => document.documentElement.scrollWidth - innerWidth);
  if (ov > 2) bad.push('settings overflow by ' + ov + 'px at 390');
  // a token, from the page
  await p.evaluate(() => document.querySelector('[data-ctab="access"]').click());
  await p.evaluate(() => document.querySelector('[data-ac="tok-new"]').click());
  await wait(p, () => document.getElementById('dlg').open, 3000);
  await p.click('#dyes');
  await wait(p, () => !!document.querySelector('[data-ac="g-new"]:not([disabled])') , 3000);
  let tok = '';
  try { tok = JSON.parse(fs.readFileSync(cfgFile, 'utf8')).token || ''; } catch (e) {}
  if (tok.length !== 32) bad.push('no token in config.json');
  const ck = (await ctx.cookies()).filter(c => c.name === 'nwtok')[0];
  if (!ck || ck.value !== tok || !ck.httpOnly) bad.push('no HttpOnly token cookie: ' + JSON.stringify(ck));
  // the page still works with it, images and all
  await p.reload({ waitUntil: 'domcontentloaded' });
  await p.waitForTimeout(1200);
  if (await p.evaluate(() => document.getElementById('dlg').open)) bad.push('asked for the token it has');
  await p.evaluate(() => document.querySelector('[data-ctab="access"]').click());
  await p.evaluate(() => document.querySelector('[data-ac="g-new"]').click());
  await wait(p, () => !!document.querySelector('[data-ac="g-rev"]'), 3000);
  let guest = '';
  try { guest = JSON.parse(fs.readFileSync(cfgFile, 'utf8')).guestToken || ''; } catch (e) {}
  if (guest.length !== 32) bad.push('no guest token');
  // the QR, drawn with no internet, for the guest link
  await p.evaluate(() => document.querySelector('#v-settings [data-ac="qr"]').click());
  await wait(p, () => !!document.querySelector('dialog [data-qk="guest"]'), 2000);
  await p.evaluate(() => document.querySelector('dialog [data-qk="guest"]').click());
  const qr = await p.evaluate(() => { const d = [...document.querySelectorAll('dialog')].pop(); return { svg: !!d.querySelector('svg path[d^="M"]'), text: d.textContent }; });
  if (!qr.svg) bad.push('no QR drawn');
  if (!qr.text.includes('?token=' + guest)) bad.push('QR text without the guest token');
  // a guest
  const gctx = await b.newContext({ viewport: { width: 1400, height: 900 } });
  const g = await gctx.newPage();
  g.on('pageerror', e => bad.push('guest: ' + e.message));
  await g.goto(BASE + '?token=' + guest + '#library', { waitUntil: 'domcontentloaded' });
  await g.waitForTimeout(1500);
  if (/token=/.test(await g.evaluate(() => location.href))) bad.push('token left in the address bar');
  if (!await g.evaluate(() => document.documentElement.classList.contains('guest'))) bad.push('not in guest mode');
  const nav = await g.evaluate(() => [...document.querySelectorAll('#nav [data-v]')].map(x => x.getAttribute('data-v')));
  for (const v of ['files', 'payloads', 'install', 'system', 'remote']) if (nav.includes(v)) bad.push('guest sees ' + v);
  if (!nav.includes('library')) bad.push('guest has no library');
  // covers are plain <img>: they get in with the cookie
  const icon = await g.evaluate(() => fetch('/apps/icon?id=PPSA01234').then(r => r.status));
  if (icon !== 200) bad.push('guest: a cover without the header answered ' + icon);
  await g.evaluate(() => { location.hash = 'files' });
  await g.waitForTimeout(400);
  if (await g.evaluate(() => !document.getElementById('v-files').hidden)) bad.push('guest opened files');
  await g.evaluate(() => { location.hash = 'settings' });
  await g.waitForTimeout(400);
  if (await g.evaluate(() => !!document.querySelector('[data-ctab="neohen"]'))) bad.push('guest has NeoHEN settings');
  await g.close();
  // a wrong token: asked for
  const n = await b.newPage();
  await n.goto(BASE + '?token=nope', { waitUntil: 'domcontentloaded' });
  if (!await wait(n, () => document.getElementById('dlg').open, 3000)) bad.push('not asked for the token');
  await n.close();
  // back to no token
  await p.evaluate(() => [...document.querySelectorAll('dialog')].forEach(d => d.open && d.close()));
  await p.evaluate(() => document.querySelector('[data-ac="tok-clr"]').click());
  await wait(p, () => document.getElementById('dlg').open, 3000);
  await p.click('#dyes');
  await p.waitForTimeout(600);
  try { if (JSON.parse(fs.readFileSync(cfgFile, 'utf8')).token) bad.push('token not cleared'); } catch (e) {}
  if (cdn.some(u => /cdnjs|qrcode/.test(u))) bad.push('QR library fetched: ' + cdn.join(' '));
  await b.close();
  console.log(JSON.stringify(bad));
})().catch(e => { console.log(JSON.stringify(['crash: ' + e.message])); process.exit(0) });
"""


if __name__ == "__main__":
    unittest.main()
