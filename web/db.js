// Talks to build\minirdb.exe as a subprocess: pipes SQL to stdin (non-interactive
// mode, since stdin isn't a tty), reads the pipe-table output back from stdout.
// All calls go through a single queue because miniRDB has no multi-connection
// concurrency support (one process/file-handle at a time).

const { spawn } = require("child_process");
const path = require("path");

const EXE = path.join(__dirname, "..", "build", "minirdb.exe");
const DB_FILE = path.join(__dirname, "..", "mydata.db");
const REPO_ROOT = path.join(__dirname, "..");

let queue = Promise.resolve();

// Runs `fn` only after every previously queued job has settled, so DB access
// from this process is always sequential.
function withLock(fn) {
  const run = queue.then(fn, fn);
  queue = run.then(
    () => {},
    () => {}
  );
  return run;
}

function runMiniRdb(sql) {
  return new Promise((resolve, reject) => {
    const proc = spawn(EXE, [DB_FILE], { cwd: REPO_ROOT });
    let stdout = "";
    let stderr = "";
    proc.stdout.on("data", (d) => (stdout += d));
    proc.stderr.on("data", (d) => (stderr += d));
    proc.on("error", reject);
    proc.on("close", () => resolve({ stdout, stderr }));
    proc.stdin.write(sql);
    proc.stdin.end();
  });
}

// Parses miniRDB's script-mode output: a SELECT prints one
// "header \n ----- \n rows..." table; INSERT/UPDATE/DELETE/CREATE print
// nothing on success; any failure prints a line starting with "error:" or
// "parse error:".
function parseOutput(stdout) {
  const lines = stdout.replace(/\r\n/g, "\n").split("\n").filter((l) => l.length > 0);
  const errorLine = lines.find((l) => l.startsWith("error:") || l.startsWith("parse error:"));
  if (errorLine) return { error: errorLine };
  if (lines.length < 2) return { columns: [], rows: [] };
  const columns = lines[0].split(" | ").map((s) => s.trim());
  const rows = lines.slice(2).map((l) => l.split(" | ").map((s) => s.trim()));
  return { columns, rows };
}

async function exec(sql) {
  const { stdout, stderr } = await runMiniRdb(sql);
  const parsed = parseOutput(stdout);
  if (parsed.error) throw new Error(parsed.error);
  if (stderr.trim()) throw new Error(stderr.trim());
  return parsed;
}

// Wraps a string as a miniRDB TEXT literal, doubling embedded quotes (the
// lexer's escape convention for `'`).
function sqlString(value) {
  return "'" + String(value ?? "").replace(/'/g, "''") + "'";
}

const MEMBER_COLUMNS = ["id", "username", "name", "email", "phone", "address", "birth_date", "created_at"];

function rowToMember(row) {
  const m = {};
  MEMBER_COLUMNS.forEach((col, i) => (m[col] = row[i]));
  return m;
}

async function findByUsername(username) {
  const res = await withLock(() => exec(`SELECT id FROM members WHERE username = ${sqlString(username)};`));
  return res.rows.length > 0;
}

async function insertMember({ username, passwordHash, name, email, phone, address, birth_date }) {
  return withLock(async () => {
    const maxRes = await exec("SELECT MAX(id) FROM members;");
    const maxVal = maxRes.rows[0] && maxRes.rows[0][0];
    const nextId = maxVal === "NULL" || maxVal === undefined ? 1 : parseInt(maxVal, 10) + 1;
    const createdAt = new Date().toISOString();
    const sql =
      `INSERT INTO members (id, username, password, name, email, phone, address, birth_date, created_at) ` +
      `VALUES (${nextId}, ${sqlString(username)}, ${sqlString(passwordHash)}, ${sqlString(name)}, ` +
      `${sqlString(email)}, ${sqlString(phone)}, ${sqlString(address)}, ${sqlString(birth_date)}, ${sqlString(createdAt)});`;
    await exec(sql);
    return nextId;
  });
}

async function listMembers() {
  const res = await withLock(() =>
    exec(`SELECT id, username, name, email, phone, address, birth_date, created_at FROM members ORDER BY id;`)
  );
  return res.rows.map(rowToMember);
}

async function deleteMember(id) {
  const n = parseInt(id, 10);
  if (!Number.isInteger(n)) throw new Error("invalid id");
  return withLock(() => exec(`DELETE FROM members WHERE id = ${n};`));
}

module.exports = { insertMember, listMembers, deleteMember, findByUsername };
