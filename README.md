# miniRDB

A small disk-based relational database engine written from scratch in C —
page cache, B-Tree table/index storage, a SQL parser, a query executor with
a rule-based optimizer, and rollback-journal transactions. No external
dependencies.

## Build

Requires MSVC (Build Tools or Visual Studio) or gcc/MinGW.

```powershell
.\build.ps1
```

Produces `build\minirdb.exe`.

## Run

```powershell
.\build\minirdb.exe mydata.db
```

Opens (or creates) `mydata.db` and starts an interactive prompt. SQL
statements end with `;`. Meta-commands:

- `.tables` — list tables
- `.schema [table]` — show CREATE statements
- `.exit` / `.quit`

To run a script non-interactively:

```powershell
.\build\minirdb.exe mydata.db tests\smoke.sql
```

## Supported SQL

- `CREATE TABLE t (col TYPE [PRIMARY KEY] [NOT NULL], ...)` — types `INT`,
  `REAL`, `TEXT`. A single `INT PRIMARY KEY` column aliases the row id
  (like SQLite); tables without one get a hidden auto-increment row id.
- `DROP TABLE t`
- `CREATE INDEX name ON t(col)` / `DROP INDEX name`
- `INSERT INTO t [(cols)] VALUES (...), (...)`
- `SELECT cols|* FROM t [JOIN t2 ON cond ...] [WHERE cond] [ORDER BY col [ASC|DESC], ...] [LIMIT n]`
  — expressions support `+ - * /`, comparisons, `AND/OR/NOT`, parentheses,
  and `COUNT/SUM/AVG/MIN/MAX` (aggregates only, no `GROUP BY`, and a select
  list must be all-aggregate or all-plain, not mixed).
- `UPDATE t SET col = expr, ... [WHERE cond]`
- `DELETE FROM t [WHERE cond]`
- `BEGIN [TRANSACTION]` / `COMMIT` / `ROLLBACK` — statements outside an
  explicit `BEGIN` run in autocommit (one implicit transaction per
  statement, so a failing multi-row `INSERT` rolls back entirely).

The optimizer uses an index for `WHERE col = literal` / `JOIN ... ON a.col =
b.col` when a matching index exists on the probed column; otherwise it
falls back to a full table scan.

## Web app (member registration)

`web/` is a small Node.js/Express app that lets you register and manage rows
in a `members` table through a browser form instead of the SQL REPL.

```powershell
cd web
npm install
npm start
```

Then open http://localhost:3000. It talks to `build\minirdb.exe` as a
subprocess (piping SQL over stdin) against `mydata.db`, hashes passwords with
bcrypt before storing them, and serializes all DB access since miniRDB has no
multi-connection support. The `members` table (created once, already present
in `mydata.db`):

```sql
CREATE TABLE members (
  id INT PRIMARY KEY,
  username TEXT NOT NULL,
  password TEXT NOT NULL,
  name TEXT NOT NULL,
  email TEXT,
  phone TEXT,
  address TEXT,
  birth_date TEXT,
  created_at TEXT
);
CREATE INDEX idx_members_username ON members(username);
```

## Architecture

See `src/`: `pager` (4KB pages, in-memory cache, free list) → `btree`
(B+Tree table/index storage with page splits, no delete-rebalancing) →
`record`/`catalog` (row serialization, schema storage) → `lexer`/`parser`/
`ast` (SQL front end) → `executor` (row evaluation, joins, aggregates,
sorting, the optimizer) → `txn` (rollback-journal transactions with crash
recovery) → `main` (REPL).

## Known limitations

A deliberately "mini" scope: no `GROUP BY`/`HAVING`, subqueries, `ALTER
TABLE`, views/triggers, foreign key enforcement, `BLOB`/date types,
multi-connection concurrency, or WAL mode. B-Tree deletes don't rebalance
underflowing pages (space isn't reclaimed within a page, though whole pages
are freed when tables/indexes are dropped). Rows and single index keys must
fit in one 4KB page (a few KB of text max). Parse errors may leak the
partially-built AST (acceptable for a REPL; not a long-running-process
concern).
