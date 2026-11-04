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
  "game=CUSA00001","install","apps","payloads","themes","remote","files","transfers","procs","system","settings"];
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


if __name__ == "__main__":
    unittest.main()
