// w3p-factory — batch config + ledger persistence and ICCID validation.
//
// A batch lives in <toolroot>/batches/<batch-id>/ :
//   config.json    operator-editable settings (server, ports, paths)
//   ledger.json    tool-owned state machine (never edit by hand)
//   manifests/     per-ICCID cleartext manifests pulled from the server (0600)
//   blobs/         per-ICCID prov NVS images (0600, delete after verify)
//   logs/          serial + command logs
import { existsSync, mkdirSync, readdirSync, readFileSync, chmodSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { atomicWriteJson, readJson, nowIso } from './util.mjs';

export const TOOL_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
export const BATCHES_DIR = join(TOOL_ROOT, 'batches');

// Unit pipeline stages, in order. A unit is at the highest stage it has
// completed; stations (ch32x/rp2040) are counted separately because units
// have no identity there (identity = SIM, first seen at the ESP32 station).
export const STAGES = ['pending', 'provisioned', 'blob', 'flashed', 'verified'];

export function stageOf(unit) {
  for (let i = STAGES.length - 1; i >= 1; i--) {
    if (unit[STAGES[i]]) return STAGES[i];
  }
  return 'pending';
}

// ---- ICCID handling ----------------------------------------------------
// Mirror firmware modem.c + provision-device.mjs: a 20-digit ICCID has its
// 20th digit dropped (no Luhn check); the canonical form is 19 digits.
export function normalizeIccid(raw) {
  const d = String(raw).replace(/\D/g, '');
  if (d.length === 20) return d.slice(0, 19);
  if (d.length === 19) return d;
  return null;
}

// Parse an ICCID list file: one ICCID per line, blank lines and #-comments
// allowed. Returns { iccids, errors } — errors are human-readable strings.
export function parseIccidList(text) {
  const iccids = [];
  const errors = [];
  const seen = new Map(); // normalized -> first line number
  const lines = text.split('\n');
  for (let n = 0; n < lines.length; n++) {
    const line = lines[n].trim();
    if (!line || line.startsWith('#')) continue;
    const norm = normalizeIccid(line);
    if (!norm) {
      errors.push(`line ${n + 1}: not a 19-20 digit ICCID: "${line}"`);
      continue;
    }
    if (seen.has(norm)) {
      errors.push(`line ${n + 1}: duplicate of line ${seen.get(norm)} (${norm})`);
      continue;
    }
    seen.set(norm, n + 1);
    iccids.push(norm);
  }
  return { iccids, errors };
}

// ---- defaults ----------------------------------------------------------
// Paths are resolved relative to the mono-repo layout this tool lives in;
// all of them are operator-editable in config.json afterwards.
function defaultConfig(batchId) {
  const fwRepo = resolve(TOOL_ROOT, '..', '..'); // Web3-Pi-UPS/
  const monoRoot = resolve(fwRepo, '..');
  return {
    batch: batchId,
    createdAt: nowIso(),
    operator: 'robert',
    onenceAccount: 'web3pi-1nce-prod',
    server: 'root@5.75.237.218',
    containerFilter: 'api', // docker ps name filter for the panel API container
    panelRepo: join(monoRoot, 'Web3-Pi-UPS-Panel'),
    fwEsp32: join(fwRepo, 'firmware-ESP32-LTE-M'),
    fwRp2040: join(fwRepo, 'firmware-rp2040'),
    fwCh32x: join(fwRepo, 'firmware-ch32x'),
    esp32Port: '/dev/cu.usbmodem101',
    idfPython: `${process.env.HOME}/.espressif/tools/python/v6.0/venv/bin/python`,
    idfActivate: `${process.env.HOME}/.espressif/tools/activate_idf_v6.0.sh`,
    pio: `${process.env.HOME}/.platformio/penv/bin/pio`,
    rp2040Openocd: `${process.env.HOME}/.platformio/packages/tool-openocd-rp2040-earlephilhower/bin/openocd`,
    rp2040OpenocdScripts: `${process.env.HOME}/.platformio/packages/tool-openocd-rp2040-earlephilhower/openocd/scripts`,
    provOffset: '0x310000', // MUST match partitions.csv `prov` offset
    serialVerifyTimeoutS: 150, // modem bring-up can take ~40s+ (PWRKEY quirks)
    // Fleet-wide command-signing material, learned from the first real
    // provision run and cross-checked on every later one (ADR-0009).
    fleet: { bkOpPub: null, bkRootPub: null, bkEpoch: null },
  };
}

function defaultLedger(batchId, iccids) {
  const units = {};
  for (const i of iccids) units[i] = {};
  return {
    batch: batchId,
    createdAt: nowIso(),
    iccidOrder: iccids, // original list order, used for reports/labels
    units,
    stations: {
      ch32x: { done: 0, target: iccids.length, log: [] },
      rp2040: { done: 0, target: iccids.length, log: [] },
    },
    builds: {}, // frozen build fingerprints: esp32App, ch32xHex, rp2040Env
  };
}

// ---- batch create/load -------------------------------------------------
export function createBatch(batchId, iccids) {
  if (!/^[A-Za-z0-9._-]+$/.test(batchId)) {
    throw new Error(`batch id must be filesystem-safe (got "${batchId}")`);
  }
  const dir = join(BATCHES_DIR, batchId);
  if (existsSync(join(dir, 'ledger.json'))) {
    throw new Error(`batch "${batchId}" already exists at ${dir} — refusing to overwrite its ledger`);
  }
  mkdirSync(join(dir, 'manifests'), { recursive: true });
  mkdirSync(join(dir, 'blobs'), { recursive: true });
  mkdirSync(join(dir, 'logs'), { recursive: true });
  chmodSync(dir, 0o700);
  atomicWriteJson(join(dir, 'config.json'), defaultConfig(batchId));
  atomicWriteJson(join(dir, 'ledger.json'), defaultLedger(batchId, iccids));
  return loadBatch(batchId);
}

export function listBatches() {
  if (!existsSync(BATCHES_DIR)) return [];
  return readdirSync(BATCHES_DIR, { withFileTypes: true })
    .filter((d) => d.isDirectory() && existsSync(join(BATCHES_DIR, d.name, 'ledger.json')))
    .map((d) => d.name);
}

// Load by explicit id, or auto-select when exactly one batch exists.
export function loadBatch(batchId) {
  let id = batchId;
  if (!id) {
    const all = listBatches();
    if (all.length === 0) throw new Error('no batches found — run `w3p-factory init` first');
    if (all.length > 1) throw new Error(`multiple batches exist (${all.join(', ')}) — pass --batch <id>`);
    id = all[0];
  }
  const dir = join(BATCHES_DIR, id);
  if (!existsSync(join(dir, 'ledger.json'))) throw new Error(`batch "${id}" not found in ${BATCHES_DIR}`);
  const config = readJson(join(dir, 'config.json'));
  const ledger = readJson(join(dir, 'ledger.json'));

  const batch = {
    id,
    dir,
    config,
    ledger,
    paths: {
      config: join(dir, 'config.json'),
      ledger: join(dir, 'ledger.json'),
      manifests: join(dir, 'manifests'),
      blobs: join(dir, 'blobs'),
      logs: join(dir, 'logs'),
    },
    saveLedger() {
      atomicWriteJson(this.paths.ledger, this.ledger);
    },
    saveConfig() {
      atomicWriteJson(this.paths.config, this.config);
    },
    unit(iccid) {
      const u = this.ledger.units[iccid];
      if (!u) throw new Error(`ICCID ${iccid} is not part of batch ${this.id}`);
      return u;
    },
    // Record a completed stage with a timestamp + arbitrary details.
    // Re-entering a stage invalidates everything downstream: a reissue
    // (provisioned) obsoletes the old blob/flash/verify, a blob rebuild
    // obsoletes flash/verify, a re-flash obsoletes verify. Without this a
    // rotated unit would still report 'verified' and could ship with stale
    // secrets in NVS while the DB holds new ones.
    setStage(iccid, stage, details = {}) {
      if (!STAGES.includes(stage)) throw new Error(`unknown stage ${stage}`);
      const u = this.unit(iccid);
      u[stage] = { at: nowIso(), ...details };
      for (const s of STAGES.slice(STAGES.indexOf(stage) + 1)) delete u[s];
      this.saveLedger();
    },
    clearStage(iccid, stage) {
      delete this.unit(iccid)[stage];
      this.saveLedger();
    },
    note(iccid, text) {
      const u = this.unit(iccid);
      (u.notes ??= []).push({ at: nowIso(), text });
      this.saveLedger();
    },
    manifestPath(iccid) {
      return join(this.paths.manifests, `${iccid}.json`);
    },
    blobPath(iccid) {
      return join(this.paths.blobs, `prov-${iccid}.nvs.bin`);
    },
    manifest(iccid) {
      const p = this.manifestPath(iccid);
      if (!existsSync(p)) return null;
      return readJson(p);
    },
  };
  return batch;
}

// Validate a manifest pulled from the server before trusting it.
export function validateManifest(m, iccid) {
  const errs = [];
  if (normalizeIccid(String(m?.iccid ?? '')) !== iccid) errs.push(`iccid mismatch (${m?.iccid})`);
  if (!/^[A-Z2-9]{5}-[A-Z2-9]{5}$/.test(m?.claimToken ?? '')) errs.push('claimToken missing/malformed');
  if (!/^[0-9a-f]{64}$/i.test(m?.mqttSecretHex ?? '')) errs.push('mqttSecretHex missing/malformed');
  if (m?.arkiv && !/^[0-9a-f]{64}$/i.test(m.arkiv.privHex ?? '')) errs.push('arkiv.privHex malformed');
  if (!Number.isInteger(m?.bkEpoch) || m.bkEpoch < 1) errs.push('bkEpoch missing');
  return errs;
}
