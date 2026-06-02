// Headless screenshot + verification harness for the Trossen SDK webapp.
//
// Same setup used to capture the product-showcase screenshots: headless
// Chromium at 1440x900 driving the running dev server. Reuses the
// Playwright build that ships with the local gstack skill (no extra install)
// and the browser already cached under ~/.cache/ms-playwright.
//
// Usage (dev server must be up — `cd webapp && docker compose up`):
//   node scripts/capture-screens.mjs <scenario> [args...]
//
// Scenarios:
//   tds-194                 Open Configuration → Edit System modal, assert the
//                           submit button reads "Save Changes" (not "Create
//                           System"). Fully headless, no hardware needed.
//   monitor <sessionId>     Capture the Monitor page for a session and report
//                           the LIVE/idle badge label + episode counter. Use
//                           this DURING a live run to verify TDS-192 (counter)
//                           and AFTER Stop to verify TDS-193 (badge goes idle).
//   all                     Run every hardware-free scenario (currently tds-194).
//
// Env overrides:
//   BASE_URL          default http://localhost:5173
//   OUT_DIR           default ~/Documents/trossen_webapp_showcase/verify
//   PLAYWRIGHT_CORE   path to a playwright-core package dir (auto-detected)

import { createRequire } from 'node:module';
import { existsSync, mkdirSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

const require = createRequire(import.meta.url);

// --- locate playwright-core without adding a project dependency ----------
const PW_CANDIDATES = [
  process.env.PLAYWRIGHT_CORE,
  join(homedir(), '.claude/skills/gstack/node_modules/playwright-core'),
].filter(Boolean);
const pwDir = PW_CANDIDATES.find((p) => existsSync(p));
if (!pwDir) {
  console.error('playwright-core not found. Set PLAYWRIGHT_CORE to its package dir.');
  console.error('Looked in:\n  ' + PW_CANDIDATES.join('\n  '));
  process.exit(2);
}
const { chromium } = require(pwDir);

const BASE_URL = process.env.BASE_URL || 'http://localhost:5173';
const OUT_DIR =
  process.env.OUT_DIR || join(homedir(), 'Documents/trossen_webapp_showcase/verify');
mkdirSync(OUT_DIR, { recursive: true });

const VIEWPORT = { width: 1440, height: 900 };

// Tiny PASS/FAIL bookkeeping so the script exits non-zero on any failed
// assertion (handy for CI or a pre-PR check).
let failures = 0;
const assert = (ok, msg) => {
  console.log(`  ${ok ? '✅ PASS' : '❌ FAIL'}  ${msg}`);
  if (!ok) failures += 1;
};

async function withPage(fn) {
  const browser = await chromium.launch({ headless: true });
  try {
    const ctx = await browser.newContext({ viewport: VIEWPORT, deviceScaleFactor: 2 });
    const page = await ctx.newPage();
    page.setDefaultTimeout(15000);
    await fn(page);
  } finally {
    await browser.close();
  }
}

// TDS-194 — Edit System modal submit button label.
async function scenarioTds194(page) {
  console.log('\n[tds-194] Configuration → Edit Hardware System modal');
  await page.goto(`${BASE_URL}/configuration`, { waitUntil: 'networkidle' });

  const editBtn = page.locator('button[title="Edit system"]').first();
  await editBtn.waitFor({ state: 'visible' });
  await editBtn.click();

  // Modal is open once its title renders.
  await page.getByText('Edit Hardware System').waitFor({ state: 'visible' });

  const saveCount = await page.getByRole('button', { name: 'Save Changes' }).count();
  const createCount = await page.getByRole('button', { name: 'Create System' }).count();
  assert(saveCount === 1, 'submit button reads "Save Changes" in edit mode');
  assert(createCount === 0, 'submit button does NOT read "Create System" in edit mode');

  const out = join(OUT_DIR, 'tds-194-edit-system-modal.png');
  await page.screenshot({ path: out });
  console.log(`  📸 ${out}`);
}

// Monitor page capture — reports the badge label + episode counter so the
// operator can eyeball TDS-192 / TDS-193 against the live SDK log feed.
async function scenarioMonitor(page, sessionId, tag = 'monitor') {
  if (!sessionId) {
    console.error('monitor scenario needs a <sessionId>.');
    process.exit(2);
  }
  console.log(`\n[${tag}] Monitor page for session ${sessionId}`);
  await page.goto(`${BASE_URL}/monitor/${sessionId}`, { waitUntil: 'networkidle' });
  await page.waitForTimeout(1500); // let WS frames / phase settle

  // Connection badge: aria-label is "WebSocket status: <label>".
  const badge = page.locator('[aria-label^="WebSocket status:"]');
  const badgeLabel = (await badge.count())
    ? (await badge.first().getAttribute('aria-label'))?.replace('WebSocket status: ', '')
    : '(badge hidden — phase not_started)';

  // Episode counter cell: "<current> / <total>".
  const counter = await page
    .getByText(/^\d+\s*\/\s*\d+$/)
    .first()
    .textContent()
    .catch(() => null);

  console.log(`  badge   : ${badgeLabel}`);
  console.log(`  counter : ${counter ?? '(not found)'}`);

  const out = join(OUT_DIR, `${tag}-${sessionId.slice(0, 8)}.png`);
  await page.screenshot({ path: out, fullPage: false });
  console.log(`  📸 ${out}`);
}

// Read the live Monitor state: badge label, episode counter, status text,
// and the highest "Episode saved (N total)" the SDK log feed has reported.
async function readMonitorState(page) {
  const badgeRaw = await page
    .locator('[aria-label^="WebSocket status:"]')
    .first()
    .getAttribute('aria-label')
    .catch(() => null);
  const badge = badgeRaw ? badgeRaw.replace('WebSocket status: ', '') : '(hidden)';

  const counterTxt = await page
    .getByText(/^\d+\s*\/\s*\d+$/)
    .first()
    .textContent()
    .catch(() => null);
  const counter = counterTxt ? parseInt(counterTxt.split('/')[0].trim(), 10) : null;

  // Pull the whole Logs panel and find the SDK's authoritative saved count.
  const logText = await page
    .locator('xpath=//h2[normalize-space()="Logs"]/parent::*')
    .innerText()
    .catch(() => '');
  let savedFromLog = 0;
  for (const m of logText.matchAll(/Episode saved \((\d+) total\)/g)) {
    savedFromLog = Math.max(savedFromLog, parseInt(m[1], 10));
  }
  return { badge, counter, savedFromLog, logText };
}

// Full live drive: Configuration → Test Solo → Record → Start → sample →
// Stop. mode 'real' records to disk; mode 'dry' uses NullBackend.
async function scenarioLive(page, sessionId, mode) {
  if (!sessionId) {
    console.error('live scenario needs a <sessionId>.');
    process.exit(2);
  }
  const dry = mode === 'dry';
  const tag = dry ? 'tds-dryrun' : 'tds-192-193-live';
  console.log(`\n[${tag}] full live drive for session ${sessionId} (mode=${mode})`);

  // 1) Configuration — run the Hardware Test on the Solo so the in-memory
  //    hwStatus gate flips to ready. Everything after this uses in-app
  //    (client-side) navigation so that context survives.
  await page.goto(`${BASE_URL}/configuration`, { waitUntil: 'networkidle' });
  const soloName = page.getByText('Trossen Solo AI', { exact: true });
  await soloName.waitFor({ state: 'visible' });
  const soloCard = soloName.locator(
    'xpath=ancestor::div[.//button[contains(@title,"hardware connectivity")]][1]'
  );
  const testBtn = soloCard.locator('button[title*="hardware connectivity"]');
  const readyBtn = soloCard.getByRole('button', { name: 'Re-test' });

  // Real hardware: the SDK connection is occasionally flaky on first attempt
  // (UDP loss / TCP reset). Retry the test a couple of times, letting the
  // arm connection settle between attempts, before giving up.
  console.log('  • running Hardware Test on Solo…');
  let ready = false;
  for (let attempt = 1; attempt <= 3 && !ready; attempt++) {
    await testBtn.click();
    try {
      await readyBtn.waitFor({ timeout: 35000 });
      ready = true;
    } catch {
      console.log(`    attempt ${attempt}: not ready yet, settling 10s and retrying…`);
      await page.waitForTimeout(10000);
    }
  }
  assert(ready, 'Solo hardware test passed (gate is ready)');
  if (!ready) throw new Error('hardware test never reached ready state');
  console.log('  ✓ hardware test passed (Solo ready)');

  // 2) Record (client-side nav) → expand the session → open its Monitor.
  await page.getByRole('link', { name: 'Record' }).click();
  await page.getByText('TDS-192 Verify').first().waitFor({ state: 'visible' });
  await page.getByText('TDS-192 Verify').first().click(); // expand the card
  await page.getByRole('link', { name: 'Start Session' }).first().click();

  // 3) Monitor — Start (or Dry Run). Button is gated on the ready status we
  //    just set; wait until it's enabled, then click.
  const startBtn = page.getByRole('button', { name: dry ? 'Dry Run' : /^Start$/ });
  await startBtn.waitFor({ state: 'visible' });
  await page.waitForTimeout(500);
  console.log(`  • clicking ${dry ? 'Dry Run' : 'Start'}…`);
  await startBtn.click();

  // 4) Sample the live state. For a real run, stop once the SDK has saved
  //    STOP_AFTER episodes (mirrors the ticket: stop before completion).
  const STOP_AFTER = dry ? 99 : 2;
  const samples = [];
  let invariantViolated = false;
  let stopped = false;
  for (let i = 0; i < 40; i++) {
    await page.waitForTimeout(2000);
    const s = await readMonitorState(page);
    samples.push(s);
    console.log(
      `    t=${(i + 1) * 2}s  badge=${s.badge}  counter=${s.counter}  savedInLog=${s.savedFromLog}`
    );
    // TDS-192 invariant: the header counter must never run ahead of what the
    // SDK has actually saved.
    if (s.counter != null && s.counter > s.savedFromLog && !dry) {
      invariantViolated = true;
    }
    if (i === 1) {
      await page.screenshot({ path: join(OUT_DIR, `${tag}-recording.png`) });
    }
    if (!dry && s.savedFromLog >= STOP_AFTER) {
      console.log(`  • ${s.savedFromLog} episodes saved — clicking Stop…`);
      await page.getByRole('button', { name: /^Stop$/ }).click();
      stopped = true;
      break;
    }
    // Dry run: bail out once it reports complete.
    if (dry && /Dry run complete|session complete/i.test(s.logText)) {
      stopped = true;
      break;
    }
  }

  // 5) Capture + assert the terminal state.
  await page.waitForTimeout(2500);
  const final = await readMonitorState(page);
  const bannerStopped = await page
    .getByText(/Stopped — \d+ of \d+ episodes saved/)
    .first()
    .textContent()
    .catch(() => null);
  const bannerComplete = await page
    .getByText(/Dry run complete|episodes recorded — session complete/i)
    .first()
    .textContent()
    .catch(() => null);

  await page.screenshot({ path: join(OUT_DIR, `${tag}-stopped.png`) });
  console.log(`  📸 ${join(OUT_DIR, `${tag}-recording.png`)}`);
  console.log(`  📸 ${join(OUT_DIR, `${tag}-stopped.png`)}`);
  console.log(`  final: badge=${final.badge} counter=${final.counter} savedInLog=${final.savedFromLog}`);
  if (bannerStopped) console.log(`  banner: "${bannerStopped.trim()}"`);
  if (bannerComplete) console.log(`  banner: "${bannerComplete.trim()}"`);

  // Assertions
  if (!dry) {
    assert(!invariantViolated, 'TDS-192: counter never ran ahead of episodes saved (in any sample)');
    if (bannerStopped) {
      const n = parseInt(bannerStopped.match(/Stopped — (\d+) of/)[1], 10);
      assert(
        n === final.savedFromLog,
        `TDS-192: stopped banner count (${n}) matches SDK saved count (${final.savedFromLog})`
      );
    } else {
      assert(false, 'TDS-192: expected a "Stopped — N of M episodes saved" banner');
    }
  }
  assert(
    final.badge === 'idle' || final.badge === 'offline',
    `TDS-193: badge dropped LIVE after ${dry ? 'completion' : 'stop'} (now "${final.badge}")`
  );
}

const scenario = process.argv[2];
const arg = process.argv[3];
const arg2 = process.argv[4];

await withPage(async (page) => {
  switch (scenario) {
    case 'tds-194':
      await scenarioTds194(page);
      break;
    case 'monitor':
      await scenarioMonitor(page, arg);
      break;
    case 'live':
      await scenarioLive(page, arg, arg2 || 'real');
      break;
    case 'all':
      await scenarioTds194(page);
      break;
    default:
      console.error(
        `Unknown scenario "${scenario ?? ''}". Use: tds-194 | monitor <sessionId> | live <sessionId> [real|dry] | all`
      );
      process.exit(2);
  }
});

console.log(`\n${failures === 0 ? '✅ all assertions passed' : `❌ ${failures} assertion(s) failed`}`);
process.exit(failures === 0 ? 0 : 1);
