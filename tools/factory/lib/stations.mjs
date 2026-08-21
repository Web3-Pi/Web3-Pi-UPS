// w3p-factory — CH32X + RP2040 stations.
//
// Both MCUs carry generic (identical) firmware — no per-device secrets — so
// these stations have no ICCID identity: they just count units through
// (identity appears at the ESP32 station, where the SIM lives). Counters and
// a timestamped log land in ledger.stations.
import { existsSync } from 'node:fs';
import { join } from 'node:path';
import { ask, c, confirm, die, err, info, ok, run, sha256File, warn } from './util.mjs';

async function gitFingerprint(repoDir) {
  const head = await run('git', ['-C', repoDir, 'rev-parse', 'HEAD'], { timeoutMs: 15_000 });
  const dirty = await run('git', ['-C', repoDir, 'status', '--porcelain'], { timeoutMs: 15_000 });
  if (head.code !== 0) return null;
  return { commit: head.stdout.trim(), dirty: dirty.stdout.trim().length > 0 };
}

function recordStation(batch, station, extra = {}) {
  const st = batch.ledger.stations[station];
  st.done += 1;
  st.log.push({ at: new Date().toISOString(), n: st.done, ...extra });
  batch.saveLedger();
  ok(`${station}: unit ${c.bold(`${st.done}/${st.target}`)} done`);
}

async function stationLoop(batch, station, banner, flashUnit) {
  const st = batch.ledger.stations[station];
  info(`${station} station — ${st.done}/${st.target} done so far`);
  for (;;) {
    console.log('');
    const a = await ask(`${banner} ${c.dim('[Enter=flash next, q=quit]')} `);
    if (a.toLowerCase() === 'q') break;
    const res = await flashUnit();
    if (res.ok) recordStation(batch, station, res.extra);
    else err(`${station}: unit FAILED — not counted. ${res.hint ?? ''}`);
  }
  ok(`${station} station closed (${st.done}/${st.target})`);
}

// ---- CH32X -------------------------------------------------------------
// Wraps firmware-ch32x/flash.sh, which polls `wchisp probe` until the chip
// shows up in USB ISP boot mode — so the operator order is: connect, hold
// BOOT + reset, then Enter here. Mode --first (Robert 2026-08-21): fresh
// chips need the unprotect path; readout protection deliberately not used
// (firmware is open source).
export async function cmdFlashCh32x(batch, opts) {
  const cfg = batch.config;
  const flashSh = join(cfg.fwCh32x, 'flash.sh');
  const hex = join(cfg.fwCh32x, 'obj', 'USB-PD.hex');
  const mode = opts.mode ?? 'first';
  if (!['first', 'prod', 'dev'].includes(mode)) die(`--mode must be first|prod|dev (got ${mode})`);
  if (!existsSync(flashSh)) die(`flash.sh not found at ${flashSh}`);
  if (!existsSync(hex)) die(`missing ${hex} — build the project in MounRiver Studio 2 first`);
  const wchisp = await run('wchisp', ['--version'], { timeoutMs: 10_000 }).catch(() => null);
  if (!wchisp || wchisp.code !== 0) die('wchisp not runnable (expected on PATH, e.g. ~/.cargo/bin/wchisp)');

  // Freeze the hex for the batch, same discipline as the ESP32 app.
  const sha = sha256File(hex);
  const frozen = batch.ledger.builds.ch32xHex;
  if (!frozen) {
    info(`freezing CH32X hex for this batch: sha256 ${sha}`);
    if (!(await confirm('freeze THIS hex for the whole batch?'))) die('aborted');
    batch.ledger.builds.ch32xHex = { sha256: sha, frozenAt: new Date().toISOString() };
    batch.saveLedger();
  } else if (frozen.sha256 !== sha) {
    die(`obj/USB-PD.hex sha256 changed vs the frozen batch build (${frozen.sha256.slice(0, 12)}…) — restore it or re-freeze deliberately (delete builds.ch32xHex from ledger.json)`);
  }

  await stationLoop(
    batch,
    'ch32x',
    `connect unit, put CH32X into USB ISP boot (hold BOOT, tap reset).`,
    async () => {
      // flash.sh's modes are `--first`, `--prod`, and BARE `dev` (or no arg).
      const modeArg = mode === 'dev' ? 'dev' : `--${mode}`;
      const r = await run('bash', [flashSh, modeArg], { cwd: cfg.fwCh32x, timeoutMs: 300_000, echo: true });
      if (r.timedOut) return { ok: false, hint: 'timed out waiting for the chip in boot mode' };
      return r.code === 0 ? { ok: true, extra: { mode } } : { ok: false };
    },
  );
}

// ---- RP2040 ------------------------------------------------------------
// SWD via Raspberry Pi Debug Probe on J401; the board must be SELF-POWERED
// (the probe supplies no 3.3 V). pio builds + uploads in one step, so the
// "frozen build" here is the git commit of firmware-rp2040 at station start.
export async function cmdFlashRp2040(batch, opts) {
  const cfg = batch.config;
  if (!existsSync(cfg.pio)) die(`pio not found at ${cfg.pio}`);
  if (!existsSync(cfg.fwRp2040)) die(`firmware-rp2040 not found at ${cfg.fwRp2040}`);

  const fp = await gitFingerprint(cfg.fwRp2040);
  const frozen = batch.ledger.builds.rp2040;
  if (fp) {
    if (!frozen) {
      info(`freezing RP2040 source for this batch: ${fp.commit.slice(0, 12)}${fp.dirty ? c.yellow(' (DIRTY tree!)') : ''}`);
      if (fp.dirty) warn('working tree has uncommitted changes — the flashed build may not be reproducible');
      if (!(await confirm('freeze this state for the whole batch?'))) die('aborted');
      batch.ledger.builds.rp2040 = { ...fp, frozenAt: new Date().toISOString() };
      batch.saveLedger();
    } else if (frozen.commit !== fp.commit || fp.dirty !== frozen.dirty) {
      warn(`firmware-rp2040 state differs from the frozen one (${frozen.commit.slice(0, 12)}${frozen.dirty ? ' dirty' : ''} → ${fp.commit.slice(0, 12)}${fp.dirty ? ' dirty' : ''})`);
      if (!(await confirm('continue anyway?'))) die('aborted');
    }
  }

  // Optional SWD link preflight (expect DPIDR 0x0bc12477 + both cores).
  const ocd = cfg.rp2040Openocd;
  if (ocd && existsSync(ocd)) {
    info('SWD link test…');
    const r = await run(ocd, ['--search', cfg.rp2040OpenocdScripts, '-f', 'interface/cmsis-dap.cfg', '-f', 'target/rp2040.cfg', '-c', 'init; halt; exit'], { timeoutMs: 30_000 });
    if (r.code === 0 && /0x0bc12477/i.test(r.stdout + r.stderr)) ok('SWD link OK (DPIDR 0x0bc12477)');
    else warn('SWD link test failed — check: board powered? probe D-port cable on J401 (SC→SWCLK.3, SD→SWDIO.2, GND.4)?');
  }

  await stationLoop(
    batch,
    'rp2040',
    `connect probe to J401, power the board.`,
    async () => {
      const r = await run(cfg.pio, ['run', '-d', cfg.fwRp2040, '-e', 'pico_swd', '-t', 'upload'], {
        cwd: cfg.fwRp2040,
        timeoutMs: 600_000,
        echo: true,
      });
      const okOut = r.code === 0 && /(\*\* Verified OK \*\*|\[SUCCESS\])/.test(r.stdout + r.stderr);
      if (!okOut && /Too long SWD WAIT|multidrop/.test(r.stdout + r.stderr)) {
        return { ok: false, hint: 'looks like SWD: board unpowered or J401 cable (probe D-port).' };
      }
      return okOut ? { ok: true } : { ok: false };
    },
  );
}

// ---- mark: record work done before/outside the tool --------------------
export function cmdMark(batch, station, count, note) {
  const st = batch.ledger.stations[station];
  if (!st) die(`unknown station "${station}" (have: ${Object.keys(batch.ledger.stations).join(', ')})`);
  const n = Number(count);
  if (!Number.isInteger(n) || n < 0 || n > st.target) die(`count must be 0..${st.target}`);
  st.done = n;
  st.log.push({ at: new Date().toISOString(), n, mark: true, note: note ?? null });
  batch.saveLedger();
  ok(`${station}: marked ${n}/${st.target} done${note ? ` (${note})` : ''}`);
}
