// w3p-factory — step A: provision units against the PROD DB.
//
// Transport: ssh + docker exec into the Dokploy API container (decision
// 2026-08-21 — no panel/API changes, provision-device.mjs stays untouched).
// The container name is re-discovered every run (Dokploy regenerates the
// hash suffix on redeploy). Scripts are pushed fresh every run — idempotent.
//
// Per unit: always dry-run first, then the real run, then pull the cleartext
// manifest back into the batch dir. Individual unit failures (already
// claimed, DB refusal) are recorded and DON'T abort the batch. The only
// batch-aborting condition is fleet key (bk_*) inconsistency — that would
// produce fail-closed devices.
import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { atomicWrite, c, die, err, info, ok, shq, sshRun, warn } from './util.mjs';
import { validateManifest } from './state.mjs';

const SSH_TIMEOUT = 90_000;

// Tiny read-only helper pushed alongside provision-device.mjs: prints the
// devices row's public fingerprints so a recovered manifest can be checked
// against what the DB actually holds (a stale manifest here would brick the
// SIM↔secret pairing with every local check green).
const ROW_CHECK_SRC = `import pg from 'pg';
const iccid = process.argv[2] ?? '';
if (!/^\\d{19,20}$/.test(iccid)) { console.error('bad iccid'); process.exit(2); }
const c = new pg.Client({
  user: process.env.POSTGRES_USER ?? 'panel', password: process.env.POSTGRES_PASSWORD,
  database: process.env.POSTGRES_DB ?? 'panel', host: process.env.POSTGRES_HOST ?? 'localhost',
  port: Number(process.env.POSTGRES_PORT ?? '5432'),
});
await c.connect();
const r = await c.query('SELECT status, claim_token_hash, arkiv_device_pubkey FROM devices WHERE iccid = $1', [iccid]);
await c.end();
console.log(JSON.stringify(r.rows[0] ?? null));
`;

function assertSaneName(name) {
  if (!/^[A-Za-z0-9._-]+$/.test(name)) throw new Error(`unsafe container name from server: "${name}"`);
  return name;
}

export async function discoverContainer(config) {
  const r = await sshRun(config.server, `docker ps --format '{{.Names}}'`, { timeoutMs: SSH_TIMEOUT });
  if (r.code !== 0) {
    throw new Error(`cannot list containers on ${config.server} (ssh exit ${r.code}): ${r.stderr.trim()}`);
  }
  const names = r.stdout.split('\n').map((s) => s.trim()).filter(Boolean);
  const matches = names.filter((n) => n.toLowerCase().includes(config.containerFilter.toLowerCase()));
  if (matches.length === 0) {
    throw new Error(`no container matching "${config.containerFilter}" on ${config.server}. Running: ${names.join(', ')}`);
  }
  if (matches.length > 1) {
    throw new Error(`multiple containers match "${config.containerFilter}": ${matches.join(', ')} — tighten containerFilter in config.json`);
  }
  return assertSaneName(matches[0]);
}

export async function pushScripts(config, ct) {
  const scriptsDir = join(config.panelRepo, 'apps', 'api', 'scripts');
  const files = [
    { local: join(scriptsDir, 'provision-device.mjs'), remote: '/app/prov/provision-device.mjs' },
    { local: join(scriptsDir, 'lib', 'secretbox.mjs'), remote: '/app/prov/lib/secretbox.mjs' },
  ];
  for (const f of files) {
    if (!existsSync(f.local)) throw new Error(`missing local script: ${f.local} — is panelRepo in config.json correct?`);
  }
  let r = await sshRun(config.server, `docker exec -u root ${ct} mkdir -p /app/prov/lib`, { timeoutMs: SSH_TIMEOUT });
  if (r.code !== 0) throw new Error(`mkdir in container failed: ${r.stderr.trim()}`);
  for (const f of files) {
    r = await sshRun(config.server, `docker exec -iu root ${ct} sh -c 'cat > ${f.remote}'`, {
      timeoutMs: SSH_TIMEOUT,
      input: readFileSync(f.local),
    });
    if (r.code !== 0) throw new Error(`push ${f.remote} failed: ${r.stderr.trim()}`);
  }
  r = await sshRun(config.server, `docker exec -iu root ${ct} sh -c 'cat > /app/prov/row-check.mjs'`, {
    timeoutMs: SSH_TIMEOUT,
    input: ROW_CHECK_SRC,
  });
  if (r.code !== 0) throw new Error(`push /app/prov/row-check.mjs failed: ${r.stderr.trim()}`);
}

// Fetch the DB row's public fingerprints. Returns the row object, or null
// when no row exists, or undefined on transport/parse failure.
async function fetchRow(config, ct, iccid) {
  const r = await sshRun(config.server, `docker exec -u root -w /app ${ct} node prov/row-check.mjs ${iccid}`, {
    timeoutMs: SSH_TIMEOUT,
  });
  if (r.code !== 0) return undefined;
  try {
    return JSON.parse(r.stdout);
  } catch {
    return undefined;
  }
}

// A recovered manifest is only trustworthy if it matches the live DB row:
// sha256(claimToken) must equal claim_token_hash, and when both sides carry
// an Arkiv pubkey they must agree.
function manifestMatchesRow(manifest, row) {
  if (!row?.claim_token_hash) return false;
  const hash = createHash('sha256').update(manifest.claimToken, 'utf8').digest('hex');
  if (hash !== row.claim_token_hash.toLowerCase()) return false;
  if (row.arkiv_device_pubkey && manifest.arkiv?.pubHex && row.arkiv_device_pubkey.toLowerCase() !== manifest.arkiv.pubHex.toLowerCase()) return false;
  return true;
}

function provisionCmd(config, ct, iccid, extra = []) {
  const args = [
    'node', 'prov/provision-device.mjs', iccid,
    '--batch', shq(config.batch),
    '--account', shq(config.onenceAccount),
    '--operator', shq(config.operator),
    ...extra,
  ];
  return `docker exec -u root -w /app ${ct} ${args.join(' ')}`;
}

// Interpret provision-device.mjs output. Returns one of:
//   { kind: 'fresh' } | { kind: 'exists', status } | { kind: 'claimed' } |
//   { kind: 'error', message }
function interpretDryRun(r) {
  const text = r.stdout + r.stderr;
  if (r.code === 0 && /\[dry-run\].*absent.*INSERT/s.test(text)) return { kind: 'fresh' };
  if (r.code === 0 && /\[dry-run\].*exists \(status=([a-z_]+)\)/s.test(text)) {
    return { kind: 'exists', status: text.match(/exists \(status=([a-z_]+)\)/)[1] };
  }
  if (/already CLAIMED/i.test(text)) return { kind: 'claimed' };
  if (/already provisioned \(status=([a-z_]+)\)/.test(text)) {
    return { kind: 'exists', status: text.match(/already provisioned \(status=([a-z_]+)\)/)[1] };
  }
  return { kind: 'error', message: (r.stderr || r.stdout).trim() || `exit ${r.code}` };
}

// Extract fleet-wide bk_* from the emitted make-nvs-blob command and check
// consistency against config.fleet (first run seeds it). Throws on mismatch
// or TODO placeholders — both would produce fail-closed devices.
function extractAndCheckFleet(batch, stderr) {
  if (stderr.includes('<TODO')) {
    throw new Error('server emitted <TODO> bk_* keys — CMD_SIGNING_KEY/CMD_ROOT_PUBKEY missing in container env. STOP: a blob built from this would fail-closed reject all backend commands.');
  }
  const op = stderr.match(/--bk-op-pub (04[0-9a-fA-F]{128})/)?.[1]?.toLowerCase();
  const root = stderr.match(/--bk-root-pub (04[0-9a-fA-F]{128})/)?.[1]?.toLowerCase();
  const epoch = Number(stderr.match(/--bk-epoch (\d+)/)?.[1]);
  if (!op || !root || !Number.isInteger(epoch)) {
    throw new Error('could not parse bk_op_pub/bk_root_pub/bk_epoch from server output — provision-device.mjs output format changed?');
  }
  const fleet = batch.config.fleet;
  if (!fleet.bkOpPub) {
    batch.config.fleet = { bkOpPub: op, bkRootPub: root, bkEpoch: epoch };
    batch.saveConfig();
    info(`fleet keys learned: bk_epoch=${epoch}, bk_op_pub=${op.slice(0, 12)}…, bk_root_pub=${root.slice(0, 12)}…`);
  } else if (fleet.bkOpPub !== op || fleet.bkRootPub !== root || fleet.bkEpoch !== epoch) {
    throw new Error('FLEET KEY MISMATCH between provision runs — backend signing keys changed mid-batch. Stop and investigate before flashing anything.');
  }
}

async function pullManifest(batch, ct, iccid) {
  const r = await sshRun(batch.config.server, `docker exec -u root ${ct} cat /app/out/provision/${iccid}.json`, {
    timeoutMs: SSH_TIMEOUT,
  });
  let raw = r.code === 0 ? r.stdout : null;
  let source = 'container';
  if (raw === null) {
    // Container redeploys wipe /app/out — fall back to a manifest saved by an
    // earlier manual provisioning session in the panel repo checkout.
    const local = join(batch.config.panelRepo, 'apps', 'api', 'out', 'provision', `${iccid}.json`);
    if (existsSync(local)) {
      raw = readFileSync(local, 'utf8');
      source = 'panel-repo';
    } else {
      return { ok: false, error: r.stderr.trim() || 'no manifest in container nor in local panel repo' };
    }
  }
  let m;
  try {
    m = JSON.parse(raw);
  } catch {
    return { ok: false, error: `manifest from ${source} is not valid JSON` };
  }
  const errs = validateManifest(m, iccid);
  if (errs.length) return { ok: false, error: `manifest from ${source} invalid: ${errs.join('; ')}` };
  atomicWrite(batch.manifestPath(iccid), JSON.stringify(m, null, 2) + '\n');
  return { ok: true, manifest: m, source };
}

// ---- the command -------------------------------------------------------
// opts: { iccid?: only this unit, dryRun: stop after dry-run (no writes),
//         reissue: allow rotating an existing unclaimed unit }
export async function cmdProvision(batch, opts) {
  const cfg = batch.config;
  if (opts.reissue && !opts.iccid) {
    die('--reissue requires --iccid <one unit> — bulk secret rotation is never what you want mid-production');
  }
  const targets = (opts.iccid ? [opts.iccid] : batch.ledger.iccidOrder).filter((i) => {
    const u = batch.ledger.units[i];
    if (!u) die(`ICCID ${opts.iccid} is not part of this batch`);
    return opts.reissue || !u.provisioned;
  });
  if (targets.length === 0) {
    ok('nothing to do — all units already provisioned (use --iccid + --reissue to rotate one)');
    return;
  }

  info(`server ${c.bold(cfg.server)} — discovering API container…`);
  const ct = await discoverContainer(cfg);
  ok(`container: ${ct}`);
  await pushScripts(cfg, ct);
  ok('provisioning scripts pushed');
  info(`${targets.length} unit(s) to process${opts.dryRun ? c.yellow(' [DRY-RUN — no writes]') : ''}`);

  const summary = { fresh: 0, provisioned: 0, claimed: 0, existing: 0, failed: 0 };
  for (const iccid of targets) {
    const label = c.bold(iccid);
    const dry = await sshRun(cfg.server, provisionCmd(cfg, ct, iccid, ['--dry-run']), { timeoutMs: SSH_TIMEOUT });
    const verdict = interpretDryRun(dry);

    // Dry-run mode reports and touches NOTHING — no ledger writes, no
    // manifest pulls, no state advancement.
    if (opts.dryRun) {
      if (verdict.kind === 'fresh') { ok(`${label}: dry-run OK (would INSERT)`); summary.fresh++; }
      else if (verdict.kind === 'claimed') { warn(`${label}: already CLAIMED on the server — would skip`); summary.claimed++; }
      else if (verdict.kind === 'exists') { ok(`${label}: exists (status=${verdict.status}) — would recover manifest or need --reissue`); summary.existing++; }
      else { err(`${label}: dry-run failed: ${verdict.message}`); summary.failed++; }
      continue;
    }

    if (verdict.kind === 'claimed') {
      warn(`${label}: already CLAIMED on the server — skipping (needs a human decision; likely a test unit)`);
      batch.unit(iccid).serverStatus = 'claimed';
      batch.note(iccid, 'provision skipped: already claimed on server');
      summary.claimed++;
      continue;
    }
    if (verdict.kind === 'error') {
      err(`${label}: dry-run failed: ${verdict.message}`);
      summary.failed++;
      continue;
    }
    if (verdict.kind === 'exists' && !opts.reissue) {
      // Row exists (unclaimed). Try to recover its manifest instead of
      // minting new secrets — the sticker may already be printed. A
      // recovered manifest (from the container OR the panel-repo fallback)
      // could be stale (e.g. reissued in a lost session), so it must match
      // the live DB row before we trust it.
      const pulled = await pullManifest(batch, ct, iccid);
      if (pulled.ok) {
        const row = await fetchRow(cfg, ct, iccid);
        if (row === undefined) {
          err(`${label}: could not read the DB row to validate the recovered manifest — fix connectivity and re-run`);
          summary.failed++;
          continue;
        }
        if (!manifestMatchesRow(pulled.manifest, row)) {
          warn(`${label}: recovered manifest (${pulled.source}) does NOT match the DB row — it is stale. Re-run with --iccid ${iccid} --reissue to rotate its secrets (invalidates any printed sticker).`);
          batch.note(iccid, `stale manifest from ${pulled.source} rejected (DB row mismatch)`);
          summary.failed++;
          continue;
        }
        batch.setStage(iccid, 'provisioned', { reused: true, source: pulled.source, serverStatus: verdict.status });
        ok(`${label}: already provisioned (${verdict.status}) — manifest recovered from ${pulled.source}, matches DB row`);
        summary.existing++;
      } else {
        warn(`${label}: already provisioned (${verdict.status}) but no manifest recoverable (${pulled.error}). Re-run with --iccid ${iccid} --reissue to rotate its secrets (invalidates any printed sticker).`);
        batch.note(iccid, `provisioned on server but manifest unrecoverable: ${pulled.error}`);
        summary.failed++;
      }
      continue;
    }

    const real = await sshRun(
      cfg.server,
      provisionCmd(cfg, ct, iccid, verdict.kind === 'exists' ? ['--reissue'] : []),
      { timeoutMs: SSH_TIMEOUT },
    );
    if (real.code !== 0 || !/✅ provisioned/.test(real.stderr)) {
      err(`${label}: provision FAILED: ${(real.stderr || real.stdout).trim().slice(0, 400)}`);
      summary.failed++;
      continue;
    }
    extractAndCheckFleet(batch, real.stderr); // throws → aborts batch on purpose

    const pulled = await pullManifest(batch, ct, iccid);
    if (!pulled.ok) {
      err(`${label}: provisioned but manifest pull FAILED (${pulled.error}) — fix before blobs`);
      batch.note(iccid, `manifest pull failed: ${pulled.error}`);
      summary.failed++;
      continue;
    }
    batch.setStage(iccid, 'provisioned', { reissued: verdict.kind === 'exists' });
    ok(`${label}: provisioned, manifest saved (claim token ends …${pulled.manifest.claimToken.slice(-4)})`);
    summary.provisioned++;
  }

  console.log('');
  ok(`done: ${summary.provisioned} provisioned, ${summary.existing} recovered-existing, ${summary.fresh} dry-run-ok, ${summary.claimed} claimed-skipped, ${summary.failed} failed`);
  if (summary.failed > 0) process.exitCode = 1;
}
