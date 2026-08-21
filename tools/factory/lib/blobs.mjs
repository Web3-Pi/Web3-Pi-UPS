// w3p-factory — step B: build per-unit `prov` NVS images from manifests.
//
// Wraps firmware-ESP32-LTE-M/tools/make-nvs-blob (which self-sources ESP-IDF
// v6.0). Fleet-wide bk_* keys come from batch config (learned during
// `provision`, ADR-0009-consistent by construction).
//
// Verification philosophy: make-nvs-blob validates every argument's shape,
// we check the output size (must equal the 0x4000 prov partition) and record
// its sha256; the *authoritative* end-to-end check happens at the ESP32
// station, where the device's boot log must echo the manifest's secret
// prefix and the SIM's ICCID. `--deep` additionally dumps the image with
// IDF's nvs_tool for eyeball/automated cross-check (best-effort: a parse
// mismatch is a WARN, not a FAIL).
import { chmodSync, existsSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { c, die, err, info, ok, run, sha256File, warn } from './util.mjs';

const PROV_SIZE = 0x4000;

async function idfEnv(config) {
  const activate = config.idfActivate ?? `${process.env.HOME}/.espressif/tools/activate_idf_v6.0.sh`;
  const r = await run('bash', [activate, '-e'], { timeoutMs: 30_000 });
  if (r.code !== 0) return null;
  const env = {};
  for (const line of r.stdout.split('\n')) {
    const i = line.indexOf('=');
    if (i > 0) env[line.slice(0, i)] = line.slice(i + 1);
  }
  return env;
}

async function deepVerify(batch, iccid, manifest) {
  const env = await idfEnv(batch.config);
  const nvsTool = env?.IDF_PATH && join(env.IDF_PATH, 'components', 'nvs_flash', 'nvs_partition_tool', 'nvs_tool.py');
  if (!nvsTool || !existsSync(nvsTool)) {
    warn(`${iccid}: deep verify skipped (nvs_tool.py not found)`);
    return;
  }
  const r = await run(batch.config.idfPython, [nvsTool, '-d', 'blobs', batch.blobPath(iccid)], { timeoutMs: 60_000 });
  if (r.code !== 0) {
    warn(`${iccid}: deep verify inconclusive (nvs_tool exit ${r.code})`);
    return;
  }
  // `-d blobs` dumps only blob/string entries — bk_epoch is a u32 primitive
  // and never appears there, so the expected key set is derived from the
  // manifest (ak_dev_priv only for Arkiv units) and bk_epoch is checked via
  // a second `-d minimal` pass. Hexdump gutters can break secret contiguity,
  // so an inconclusive result is a WARN — the serial-stage check is the
  // authoritative one.
  const flat = r.stdout.toLowerCase().split('\n').map((l) => l.replace(/[^0-9a-f]/g, '')).join('');
  const found = flat.includes(manifest.mqttSecretHex.toLowerCase());
  const expected = ['mqtt_secret', 'bk_op_pub', 'bk_root_pub', ...(manifest.arkiv ? ['ak_dev_priv'] : [])];
  const missing = expected.filter((k) => !r.stdout.includes(k));

  const r2 = await run(batch.config.idfPython, [nvsTool, '-d', 'minimal', batch.blobPath(iccid)], { timeoutMs: 60_000 });
  const epochSeen = r2.code === 0 ? r2.stdout.match(/bk_epoch\s*[:=]\s*(\d+)/)?.[1] : null;
  const epochOk = epochSeen != null && Number(epochSeen) === manifest.bkEpoch;

  if (missing.length) {
    warn(`${iccid}: deep verify: missing key(s) in dump: [${missing.join(', ')}]`);
  } else if (found && epochOk) {
    ok(`${iccid}: deep verify OK (keys present, secret matches manifest, bk_epoch=${epochSeen})`);
  } else if (!found) {
    warn(`${iccid}: deep verify inconclusive (keys present, secret bytes not contiguously visible in dump)`);
  } else {
    warn(`${iccid}: deep verify: bk_epoch ${epochSeen ?? 'not seen'} != manifest ${manifest.bkEpoch}`);
  }
}

export async function cmdBlobs(batch, opts) {
  const cfg = batch.config;
  const fleet = cfg.fleet;
  if (!fleet?.bkOpPub || !fleet?.bkRootPub || !Number.isInteger(fleet?.bkEpoch)) {
    die('fleet bk_* keys not known yet — run `provision` first (they are learned from the server output)');
  }
  if (opts.force && !opts.iccid) {
    die('--force requires --iccid <one unit> — a bulk rebuild would invalidate the flashed/verified state of every done unit (setStage truncates downstream stages)');
  }
  const makeBlob = join(cfg.fwEsp32, 'tools', 'make-nvs-blob');
  if (!existsSync(makeBlob)) die(`make-nvs-blob not found at ${makeBlob}`);

  const targets = (opts.iccid ? [opts.iccid] : batch.ledger.iccidOrder).filter((i) => {
    const u = batch.ledger.units[i];
    if (!u) die(`ICCID ${opts.iccid} is not part of this batch`);
    if (!u.provisioned) return false;
    if (opts.force) return true; // targeted: --iccid enforced above
    if (!u.blob) return true; // never built
    // A blob deliberately deleted after the unit verified must NOT be
    // rebuilt implicitly — that would truncate the unit back to stage
    // 'blob'. Rebuild it only via an explicit --iccid --force.
    if (u.blob.deleted) return false;
    return !existsSync(batch.blobPath(i)); // file lost before flashing — rebuild
  });
  if (opts.force && batch.ledger.units[opts.iccid]?.verified) {
    warn(`${opts.iccid} is VERIFIED — rebuilding its blob resets it to stage 'blob' (it will need re-flash + re-verify)`);
  }
  if (targets.length === 0) {
    ok('nothing to do — every provisioned unit already has a blob (use --force to rebuild)');
    return;
  }
  info(`building ${targets.length} blob(s) → ${batch.paths.blobs}`);

  let built = 0;
  let failed = 0;
  for (const iccid of targets) {
    const m = batch.manifest(iccid);
    if (!m) {
      err(`${c.bold(iccid)}: no manifest in batch dir — re-run provision`);
      failed++;
      continue;
    }
    if (m.bkEpoch !== fleet.bkEpoch) {
      err(`${c.bold(iccid)}: manifest bkEpoch=${m.bkEpoch} != fleet bkEpoch=${fleet.bkEpoch} — investigate before flashing`);
      failed++;
      continue;
    }
    const out = batch.blobPath(iccid);
    const args = [
      '--secret', m.mqttSecretHex,
      '--bk-op-pub', fleet.bkOpPub,
      '--bk-root-pub', fleet.bkRootPub,
      '--bk-epoch', String(fleet.bkEpoch),
      '--iccid', iccid,
      '--out', out,
    ];
    if (m.arkiv?.privHex) args.push('--ak-dev-priv', m.arkiv.privHex);
    else warn(`${c.bold(iccid)}: manifest has no Arkiv key — building an MQTT-only blob`);

    const r = await run(makeBlob, args, { timeoutMs: 120_000 });
    if (r.code !== 0 || !existsSync(out)) {
      err(`${c.bold(iccid)}: make-nvs-blob failed (exit ${r.code}): ${r.stderr.trim().slice(0, 300)}`);
      failed++;
      continue;
    }
    chmodSync(out, 0o600); // cleartext secrets — same policy as manifests
    const size = statSync(out).size;
    if (size !== PROV_SIZE) {
      err(`${c.bold(iccid)}: blob size ${size} != expected ${PROV_SIZE} — NOT usable`);
      failed++;
      continue;
    }
    const sha = sha256File(out);
    batch.setStage(iccid, 'blob', { sha256: sha, arkiv: Boolean(m.arkiv) });
    ok(`${c.bold(iccid)}: blob built (sha256 ${sha.slice(0, 12)}…)`);
    built++;
    if (opts.deep) await deepVerify(batch, iccid, m);
  }

  console.log('');
  ok(`done: ${built} built, ${failed} failed`);
  if (failed > 0) process.exitCode = 1;
}
