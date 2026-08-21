// w3p-factory — labels CSV + batch status/evidence reporting.
import { existsSync } from 'node:fs';
import { join } from 'node:path';
import { atomicWrite, c, die, info, ok, table, warn } from './util.mjs';
import { stageOf, STAGES } from './state.mjs';

// labels-<batch>.csv: iccid,claim_token — feed for the label printer
// (labels go on the device; no QR codes — decision 2026-08-21). Cleartext
// claim tokens: 0600, gitignored, treat like the manifest CSV.
export function cmdLabels(batch, opts) {
  const rows = [];
  const missing = [];
  for (const iccid of batch.ledger.iccidOrder) {
    const m = batch.manifest(iccid);
    if (!m) {
      missing.push(iccid);
      continue;
    }
    rows.push(`${iccid},${m.claimToken}`);
  }
  if (rows.length === 0) die('no manifests in this batch yet — run `provision` first');
  const out = opts.out ?? join(batch.dir, `labels-${batch.id}.csv`);
  atomicWrite(out, 'iccid,claim_token\n' + rows.join('\n') + '\n');
  ok(`${rows.length} label row(s) → ${out}  ${c.yellow('(cleartext claim tokens — print, then guard this file)')}`);
  if (missing.length) warn(`${missing.length} unit(s) have no manifest yet: ${missing.slice(0, 5).join(', ')}${missing.length > 5 ? '…' : ''}`);
}

function unitRow(batch, iccid) {
  const u = batch.ledger.units[iccid];
  const stage = stageOf(u);
  const flags = [];
  if (u.serverStatus === 'claimed') flags.push('CLAIMED');
  if (u.provisioned?.reused) flags.push(u.provisioned.source ? `recovered:${u.provisioned.source}` : 'recovered');
  if (u.provisioned?.reissued) flags.push('reissued');
  if (u.blob?.deleted) flags.push('blob-deleted');
  const lastNote = u.notes?.length ? u.notes[u.notes.length - 1].text : '';
  return { stage, flags, rssi: u.verified?.rssi ?? '', lastNote };
}

export function cmdStatus(batch, opts) {
  const counts = Object.fromEntries(STAGES.map((s) => [s, 0]));
  const rows = [];
  for (const iccid of batch.ledger.iccidOrder) {
    const r = unitRow(batch, iccid);
    counts[r.stage]++;
    rows.push([iccid, r.stage, r.flags.join(','), r.rssi, r.lastNote.slice(0, 48)]);
  }
  const st = batch.ledger.stations;

  if (opts.json || opts.export) {
    const doc = {
      batch: batch.id,
      generatedAt: new Date().toISOString(),
      summary: counts,
      stations: { ch32x: { done: st.ch32x.done, target: st.ch32x.target }, rp2040: { done: st.rp2040.done, target: st.rp2040.target } },
      builds: batch.ledger.builds,
      units: Object.fromEntries(
        batch.ledger.iccidOrder.map((i) => {
          const u = batch.ledger.units[i];
          const r = unitRow(batch, i);
          return [i, { stage: r.stage, flags: r.flags, verifiedAt: u.verified?.at ?? null, rssi: u.verified?.rssi ?? null, notes: u.notes ?? [] }];
        }),
      ),
    };
    const text = JSON.stringify(doc, null, 2) + '\n';
    if (opts.export) {
      atomicWrite(opts.export, text, 0o644); // evidence export holds no secrets
      ok(`status exported → ${opts.export}`);
    }
    if (opts.json) process.stdout.write(text);
    return;
  }

  console.log(c.bold(`\nbatch ${batch.id}`));
  console.log(
    `  stations: ch32x ${st.ch32x.done}/${st.ch32x.target}, rp2040 ${st.rp2040.done}/${st.rp2040.target}\n` +
      `  units   : ${STAGES.map((s) => `${s} ${counts[s]}`).join(', ')}\n`,
  );
  table(['iccid', 'stage', 'flags', 'rssi', 'last note'], rows);
  const blobsLeft = batch.ledger.iccidOrder.filter((i) => existsSync(batch.blobPath(i))).length;
  if (blobsLeft) info(`${blobsLeft} cleartext blob file(s) still on disk in ${batch.paths.blobs}`);
}
