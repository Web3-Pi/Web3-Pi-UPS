// w3p-factory — ESP32 station: flash app + prov blob, then verify identity.
//
// Pairing design (2026-08-21): a bare unit CANNOT tell us its ICCID first —
// main.c ESP_ERROR_CHECK(identity_init()) aborts boot without a prov
// partition, by design. So the operator enters the ICCID (SIM-tray sticker;
// a barcode scanner acts as a keyboard), the tool validates it against the
// batch list (a typo virtually cannot hit another valid entry), flashes
// app + blob in one go, and then the DEVICE confirms identity at the end:
// the boot log must echo `identity: ICCID=<the same ICCID>` and the
// manifest's mqtt secret prefix. Mismatch = hard fail for that unit.
import { spawn } from 'node:child_process';
import { appendFileSync, closeSync, existsSync, openSync, readdirSync, readFileSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { ask, c, confirm, die, err, info, ok, run, sha256File, warn } from './util.mjs';
import { normalizeIccid, stageOf } from './state.mjs';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The station's normal rhythm is plug → flash → unplug → next unit, so the
// ESP32 port comes and goes all day. Never die on a missing port — WAIT for
// it. A port that freshly APPEARS during the wait is the just-plugged unit
// and is used automatically; a manual pick is remembered for the session.
async function waitForPort(cfg, session) {
  const listPorts = () => readdirSync('/dev').filter((f) => f.startsWith('cu.usbmodem')).map((f) => `/dev/${f}`);
  const preferred = session.port ?? cfg.esp32Port;
  if (existsSync(preferred)) return preferred;
  const background = new Set(listPorts()); // e.g. the debug probe, always plugged
  info(`waiting for the unit on USB (${preferred}) — connect it now…`);
  let promptAfter = Date.now() + 20_000;
  for (;;) {
    await sleep(1000);
    if (existsSync(preferred)) return preferred;
    const fresh = listPorts().filter((p) => !background.has(p));
    if (fresh.length === 1) {
      info(`unit appeared on ${fresh[0]}`);
      session.port = fresh[0];
      return fresh[0];
    }
    // Nothing new appeared for a while but ports exist (unit was plugged in
    // before the wait started, or several appeared at once) — let the
    // operator pick instead of guessing.
    const cands = fresh.length ? fresh : listPorts();
    if (fresh.length > 1 || (Date.now() > promptAfter && cands.length)) {
      warn(`configured ${preferred} absent; candidate port(s): ${cands.join(', ')}`);
      const a = await ask(`use which port? ${c.dim(`[Enter=${cands[0]}, w=keep waiting]`)} `);
      if (a.toLowerCase() !== 'w') {
        const picked = a.trim() || cands[0];
        if (existsSync(picked)) {
          session.port = picked;
          return picked;
        }
        err(`port ${picked} does not exist`);
      }
      promptAfter = Date.now() + 20_000;
    }
  }
}

// Frozen-build discipline: one binary per batch. The first station run
// records the app_bin sha256; every later run must match it exactly.
async function checkFrozenBuild(batch) {
  const cfg = batch.config;
  const descPath = join(cfg.fwEsp32, 'build', 'project_description.json');
  if (!existsSync(descPath)) {
    die(`no build found (${descPath} missing) — run \`tools/idf build\` in ${cfg.fwEsp32} first`);
  }
  const desc = JSON.parse(readFileSync(descPath, 'utf8'));
  const appBin = join(cfg.fwEsp32, 'build', desc.app_bin);
  if (!existsSync(appBin)) die(`app binary missing: ${appBin}`);
  const sha = sha256File(appBin);
  const frozen = batch.ledger.builds.esp32App;
  if (!frozen) {
    info(`freezing ESP32 build for this batch:`);
    info(`  ${desc.app_bin}  sha256 ${sha}`);
    if (!(await confirm('freeze THIS build for the whole batch?'))) die('aborted — build not frozen');
    batch.ledger.builds.esp32App = { file: desc.app_bin, sha256: sha, project: desc.project_name, frozenAt: new Date().toISOString() };
    batch.saveLedger();
    return;
  }
  if (frozen.sha256 !== sha) {
    die(
      `build/${desc.app_bin} sha256 ${sha.slice(0, 12)}… does not match the batch's FROZEN build ` +
        `${frozen.sha256.slice(0, 12)}… (frozen ${frozen.frozenAt}).\n` +
        `  Either restore the frozen build, or re-freeze deliberately by deleting builds.esp32App from ledger.json.`,
    );
  }
}

function stepLog(batch, iccid, name, r) {
  const p = join(batch.paths.logs, `flash-${iccid}.log`);
  appendFileSync(p, `\n===== ${new Date().toISOString()} ${name} (exit ${r.code}) =====\n${r.stdout}\n${r.stderr}\n`, { mode: 0o600 });
}

async function flashOne(batch, iccid, port, opts) {
  const cfg = batch.config;
  const idf = join(cfg.fwEsp32, 'tools', 'idf');
  const blob = batch.blobPath(iccid);
  const manifest = batch.manifest(iccid);

  const steps = [
    { name: 'erase-flash', cmd: idf, args: ['-p', port, 'erase-flash'], timeoutMs: 240_000 },
    { name: 'flash-app', cmd: idf, args: ['-p', port, 'flash'], timeoutMs: 600_000 },
    { name: 'write-prov', cmd: cfg.idfPython, args: ['-m', 'esptool', '--port', port, 'write_flash', cfg.provOffset, blob], timeoutMs: 180_000 },
    { name: 'verify-prov', cmd: cfg.idfPython, args: ['-m', 'esptool', '--port', port, 'verify_flash', cfg.provOffset, blob], timeoutMs: 180_000, expect: /Verif(y|ication) successful/i },
  ];
  for (const s of steps) {
    info(`${c.bold(iccid)}: ${s.name}…`);
    const r = await run(s.cmd, s.args, { cwd: cfg.fwEsp32, timeoutMs: s.timeoutMs, echo: true });
    stepLog(batch, iccid, s.name, r);
    if (r.code !== 0 || (s.expect && !s.expect.test(r.stdout + r.stderr))) {
      err(`${s.name} FAILED (exit ${r.code}${r.timedOut ? ', timed out' : ''}) — unit NOT flashed correctly`);
      batch.note(iccid, `flash step ${s.name} failed`);
      return false;
    }
  }
  batch.setStage(iccid, 'flashed', {
    appSha256: batch.ledger.builds.esp32App.sha256,
    blobSha256: batch.unit(iccid).blob?.sha256,
    port,
  });
  ok(`${c.bold(iccid)}: flashed (app + prov written & verified)`);
  return serialVerify(batch, iccid, port, manifest, opts);
}

// Boot the unit and require the device to confirm its identity on serial.
// A failed (re-)verification also clears any stale 'verified' stage so a
// unit can never keep reporting verified after failing a later check.
async function serialVerify(batch, iccid, port, manifest, opts) {
  const cfg = batch.config;
  if (opts['skip-serial']) {
    warn(`${iccid}: serial verification SKIPPED by flag — unit stays at stage "flashed"`);
    return true;
  }
  const fail = (noteText) => {
    batch.note(iccid, noteText);
    batch.clearStage(iccid, 'verified');
    return false;
  };
  const logPath = join(batch.paths.logs, `serial-${iccid}.log`);
  const monErrPath = join(batch.paths.logs, `monitor-${iccid}.err`);
  const monErrFd = openSync(monErrPath, 'w');
  // Isolated pid file + input FIFO so the factory monitor can never fight a
  // dev monitor over logs/serial-monitor.pid. Not passing --no-reset: the
  // monitor pulses reset on connect, which IS our clean-boot trigger.
  const mon = spawn(
    join(cfg.fwEsp32, 'tools', 'serial-monitor'),
    ['--port', port, '--log', logPath, '--truncate',
      '--pid-file', join(batch.paths.logs, 'factory-monitor.pid'),
      '--input-fifo', join(batch.paths.logs, 'factory-input.fifo')],
    { cwd: cfg.fwEsp32, stdio: ['ignore', 'ignore', monErrFd] },
  );
  let monDead = false;
  mon.on('error', () => { monDead = true; });
  mon.on('exit', (code) => { if (code !== 0 && code !== null) monDead = true; });
  const killMon = () => {
    try { mon.kill('SIGTERM'); } catch { /* already gone */ }
    try { closeSync(monErrFd); } catch { /* already closed */ }
  };
  try {
    const startedAt = Date.now();
    const wantPrefix = manifest.mqttSecretHex.slice(0, 8).toLowerCase();
    const deadline = startedAt + cfg.serialVerifyTimeoutS * 1000;
    let secretOk = false;
    let iccidSeen = null;
    let rssi = null;
    info(`${c.bold(iccid)}: waiting for boot identity (≤${cfg.serialVerifyTimeoutS}s; modem bring-up can take ~40s)…`);
    while (Date.now() < deadline) {
      await sleep(2000);
      if (monDead) {
        let detail = '';
        try { detail = readFileSync(monErrPath, 'utf8').trim().split('\n').pop() ?? ''; } catch { /* no stderr */ }
        err(`${iccid}: serial monitor DIED (${detail || 'no stderr'}) — cannot verify. Is the port free / IDF venv present?`);
        return fail('serial monitor died during verification');
      }
      let text = '';
      try {
        text = readFileSync(logPath, 'utf8');
      } catch {
        if (Date.now() - startedAt > 8000) {
          err(`${iccid}: serial monitor produced no log within 8s — cannot verify (check ${monErrPath})`);
          return fail('serial monitor produced no log file');
        }
        continue;
      }

      if (/prov partition init failed|not provisioned/i.test(text)) {
        err(`${iccid}: firmware reports NOT PROVISIONED — prov write did not take. Unit fails.`);
        return fail('serial: prov error after flashing');
      }
      const mp = text.match(/mqtt_password=([0-9a-fA-F]{8})/);
      if (mp) {
        if (mp[1].toLowerCase() !== wantPrefix) {
          err(`${iccid}: SECRET MISMATCH — device loaded mqtt_password=${mp[1]}…, manifest says ${wantPrefix}…. Wrong blob on this unit!`);
          return fail(`serial: secret prefix mismatch (${mp[1]})`);
        }
        secretOk = true;
      }
      const im = text.match(/ICCID=(\d{19,20})/);
      if (im) {
        iccidSeen = normalizeIccid(im[1]);
        if (iccidSeen !== iccid) {
          err(`${iccid}: ICCID MISMATCH — SIM in this unit is ${iccidSeen}. Wrong SIM/blob pairing! Unit fails.`);
          // The board we just erased+flashed physically belongs to iccidSeen:
          // whatever the ledger said about BOTH units is no longer true.
          batch.clearStage(iccid, 'flashed');
          if (batch.ledger.units[iccidSeen]) {
            batch.clearStage(iccidSeen, 'verified');
            batch.clearStage(iccidSeen, 'flashed');
            batch.note(iccidSeen, `board overwritten with ${iccid}'s blob during a mispaired flash — re-flash this unit`);
            warn(`unit ${iccidSeen} was physically overwritten — its ledger stages were reset; re-flash it too`);
          }
          return fail(`serial: SIM reports ${iccidSeen}`);
        }
      }
      // Informational RF reading: ignore the modem's 99 'unknown' sentinel
      // and keep the LAST real value seen.
      const rssiVals = [...text.matchAll(/rssi[= ]+(\d+)/gi)].map((m) => Number(m[1])).filter((v) => v !== 99);
      if (rssiVals.length) rssi = rssiVals[rssiVals.length - 1];
      if (secretOk && iccidSeen) break;
    }

    if (!secretOk || !iccidSeen) {
      err(
        `${iccid}: verification INCOMPLETE (secret ${secretOk ? 'OK' : 'not seen'}, ICCID ${iccidSeen ? 'OK' : 'not seen'}) ` +
          `within ${cfg.serialVerifyTimeoutS}s. Gotcha: a mute modem usually needs a FULL power-cycle of the unit. ` +
          `Power-cycle it, then re-run: flash-esp32 --verify-only ${iccid}`,
      );
      return fail(`serial verify timeout (secretOk=${secretOk}, iccid=${iccidSeen})`);
    }
    batch.setStage(iccid, 'verified', { secretPrefix: wantPrefix, rssi: rssi ? Number(rssi) : null });
    ok(`${c.bold(iccid)}: VERIFIED — device confirmed ICCID + secret${rssi ? ` (rssi ${rssi})` : ''}`);
    if (!opts['keep-blobs']) {
      try {
        unlinkSync(batch.blobPath(iccid));
        batch.unit(iccid).blob.deleted = true;
        batch.saveLedger();
        info(`${iccid}: blob deleted (manifest can rebuild it)`);
      } catch { /* keep going */ }
    }
    return true;
  } finally {
    killMon();
  }
}

// A dev serial monitor (the documented normal state of this bench Mac) owns
// the port and the default pid file — flashing/verifying would fight it.
function checkNoDevMonitor(cfg) {
  const pidFile = join(cfg.fwEsp32, 'logs', 'serial-monitor.pid');
  if (!existsSync(pidFile)) return;
  const pid = Number(readFileSync(pidFile, 'utf8').trim());
  if (!Number.isInteger(pid) || pid <= 0) return;
  try {
    process.kill(pid, 0); // liveness probe only
  } catch {
    return; // stale pid file — harmless
  }
  die(`a dev serial monitor is running (pid ${pid}, ${pidFile}) and owns the ESP32 port — stop it before the factory run (kill ${pid})`);
}

export async function cmdFlashEsp32(batch, opts) {
  checkNoDevMonitor(batch.config);
  await checkFrozenBuild(batch);
  // The port is resolved per unit (units are plugged/unplugged all day) —
  // a manual pick sticks for the whole session via `session.port`.
  const session = { port: opts.port ?? null };
  ok(`ESP32 station ready — frozen app ${batch.ledger.builds.esp32App.sha256.slice(0, 12)}… (no unit needs to be connected yet)`);

  // --verify-only <iccid>: re-run just the serial check (e.g. after the
  // power-cycle gotcha) without touching flash.
  if (opts['verify-only']) {
    const iccid = normalizeIccid(opts['verify-only']);
    const u = batch.unit(iccid);
    if (!u.flashed) die(`${iccid} was never flashed — nothing to verify`);
    const port = await waitForPort(batch.config, session);
    await serialVerify(batch, iccid, port, batch.manifest(iccid), opts);
    return;
  }

  for (;;) {
    console.log('');
    const raw = await ask(`${c.bold('ICCID')} (scan/type from SIM tray, ${c.dim('q=quit')}): `);
    if (raw.toLowerCase() === 'q' || raw === '') break;
    const iccid = normalizeIccid(raw);
    if (!iccid) { err('not a 19-20 digit ICCID'); continue; }
    if (!batch.ledger.units[iccid]) { err(`${iccid} is NOT in this batch — wrong SIM or typo`); continue; }
    const u = batch.unit(iccid);
    const stage = stageOf(u);

    if (stage === 'verified' && !opts.redo) {
      err(`${iccid} is already VERIFIED — refusing to reflash a done unit (pass --redo to override)`);
      continue;
    }
    if (!u.blob || !existsSync(batch.blobPath(iccid))) {
      if (u.blob?.deleted) err(`${iccid}: blob was deleted after verify — run \`blobs --iccid ${iccid} --force\` to rebuild`);
      else err(`${iccid} has no blob yet (stage: ${stage}) — run \`blobs\` first`);
      continue;
    }
    const blobSha = sha256File(batch.blobPath(iccid));
    if (blobSha !== u.blob.sha256) {
      err(`${iccid}: blob file sha mismatch vs ledger — rebuild with \`blobs --iccid ${iccid} --force\``);
      continue;
    }
    info(`unit ${c.bold(iccid)} (stage: ${stage}, blob ${blobSha.slice(0, 12)}…)`);
    if (!(await confirm(`flash this unit next? This ERASES its flash.`, { yes: opts.yes }))) continue;
    const port = await waitForPort(batch.config, session);
    await flashOne(batch, iccid, port, opts);
  }
  ok('ESP32 station closed');
}
