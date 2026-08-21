// w3p-factory — shared utilities. Zero-dependency (Node >= 18 stdlib only).
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync, renameSync, mkdirSync, chmodSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { createInterface } from 'node:readline';

// ---- terminal colors (honor NO_COLOR and non-tty) ----------------------
const useColor = process.stdout.isTTY && !process.env.NO_COLOR;
const wrap = (code) => (s) => (useColor ? `\x1b[${code}m${s}\x1b[0m` : String(s));
export const c = {
  red: wrap('31'),
  green: wrap('32'),
  yellow: wrap('33'),
  cyan: wrap('36'),
  bold: wrap('1'),
  dim: wrap('2'),
};

export const info = (m) => console.log(`${c.cyan('•')} ${m}`);
export const ok = (m) => console.log(`${c.green('✔')} ${m}`);
export const warn = (m) => console.log(`${c.yellow('!')} ${m}`);
export const err = (m) => console.error(`${c.red('✘')} ${m}`);
export const die = (m, code = 2) => {
  err(m);
  process.exit(code);
};

// ---- subprocess --------------------------------------------------------
// Run a command (argv array, never a shell string) and capture output.
// opts: { cwd, input, timeoutMs, echo (stream to console), env }
// Returns { code, stdout, stderr, timedOut }. Never throws on non-zero exit;
// throws only if the binary cannot be spawned.
export function run(cmd, args, opts = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(cmd, args, {
      cwd: opts.cwd,
      env: opts.env ? { ...process.env, ...opts.env } : process.env,
      stdio: [opts.input !== undefined ? 'pipe' : 'ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    let timedOut = false;
    let timer = null;
    if (opts.timeoutMs) {
      timer = setTimeout(() => {
        timedOut = true;
        child.kill('SIGTERM');
        setTimeout(() => child.kill('SIGKILL'), 3000).unref();
      }, opts.timeoutMs);
    }
    child.stdout.on('data', (d) => {
      stdout += d;
      if (opts.echo) process.stdout.write(d);
    });
    child.stderr.on('data', (d) => {
      stderr += d;
      if (opts.echo) process.stderr.write(d);
    });
    child.on('error', (e) => {
      if (timer) clearTimeout(timer);
      reject(new Error(`failed to spawn ${cmd}: ${e.message}`));
    });
    child.on('close', (code) => {
      if (timer) clearTimeout(timer);
      resolve({ code, stdout, stderr, timedOut });
    });
    if (opts.input !== undefined) {
      // If the child exits before consuming stdin (ssh auth refusal, docker
      // exec failing), Node emits an async EPIPE on the stream — swallow it
      // so the failure surfaces through the 'close' handler's exit code.
      child.stdin.on('error', () => {});
      child.stdin.write(opts.input);
      child.stdin.end();
    }
  });
}

// ssh wrapper: remote command is a single pre-built string. Everything
// interpolated into remote commands MUST come from validated inputs
// (ICCIDs are digits-only, container names are validated by callers).
export function sshRun(server, remoteCmd, opts = {}) {
  return run('ssh', ['-o', 'BatchMode=yes', server, remoteCmd], opts);
}

// Quote a string for safe embedding in a remote sh command line.
export function shq(s) {
  return `'${String(s).replace(/'/g, `'\\''`)}'`;
}

// ---- fs helpers --------------------------------------------------------
export function sha256File(path) {
  return createHash('sha256').update(readFileSync(path)).digest('hex');
}

export function readJson(path) {
  return JSON.parse(readFileSync(path, 'utf8'));
}

// Atomic write: tmp file in the same dir + rename. mode defaults to 0600
// because most files in a batch dir carry or reference secrets.
export function atomicWrite(path, data, mode = 0o600) {
  mkdirSync(dirname(path), { recursive: true });
  const tmp = join(dirname(path), `.${Date.now()}.${process.pid}.tmp`);
  writeFileSync(tmp, data, { mode });
  renameSync(tmp, path);
  chmodSync(path, mode); // rename preserves tmp mode, but be explicit
}

export function atomicWriteJson(path, obj, mode = 0o600) {
  atomicWrite(path, JSON.stringify(obj, null, 2) + '\n', mode);
}

// ---- interactive prompts ----------------------------------------------
export function ask(question) {
  const rl = createInterface({ input: process.stdin, output: process.stdout });
  return new Promise((resolve) =>
    rl.question(question, (answer) => {
      rl.close();
      resolve(answer.trim());
    }),
  );
}

export async function confirm(question, { yes = false } = {}) {
  if (yes) return true;
  const a = (await ask(`${question} ${c.dim('[y/N]')} `)).toLowerCase();
  return a === 'y' || a === 'yes';
}

// ---- tiny arg parser ---------------------------------------------------
// spec: { flags: ['dry-run', ...], opts: ['batch', ...] }
// returns { _: positionals, flagName: true, optName: 'value' }
export function parseArgs(argv, spec = {}) {
  const out = { _: [] };
  const flags = new Set(spec.flags ?? []);
  const opts = new Set(spec.opts ?? []);
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith('--')) {
      out._.push(a);
      continue;
    }
    const name = a.slice(2);
    if (flags.has(name)) {
      out[name] = true;
    } else if (opts.has(name)) {
      const v = argv[++i];
      if (v === undefined) throw new Error(`--${name} requires a value`);
      out[name] = v;
    } else {
      throw new Error(`unknown option --${name}`);
    }
  }
  return out;
}

// ---- table rendering ---------------------------------------------------
export function table(headers, rows) {
  const all = [headers, ...rows.map((r) => r.map((x) => String(x ?? '')))];
  const widths = headers.map((_, i) => Math.max(...all.map((r) => r[i].length)));
  const fmt = (r, dimmed = false) =>
    '  ' + r.map((x, i) => (dimmed ? c.dim(x.padEnd(widths[i])) : x.padEnd(widths[i]))).join('  ');
  console.log(fmt(headers.map((h) => h.toUpperCase()), true));
  for (const r of rows) console.log(fmt(r.map((x) => String(x ?? ''))));
}

export const nowIso = () => new Date().toISOString();
