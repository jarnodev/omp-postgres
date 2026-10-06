# omp-postgres

Asynchronous PostgreSQL access for [open.mp](https://open.mp), as a native server component.

- One worker thread per connection; queries on a connection run in the order they were sent.
- `$1, $2, ...` parameters are sent separately from the SQL, so bound values never need escaping and can't inject SQL.
- Callbacks run on the main thread; results are read with `pg_get_*` inside the callback.
- Failed queries call `OnPgError` (with the SQLSTATE code) instead of their callback.
- Client encoding defaults to `WIN1252`, which is what SA-MP clients send; the database itself stays UTF-8.
- Lost connections are re-established on the next query; queries that provably didn't run are retried.
- Statements built with `pg_new_query` are prepared once per connection and reused.
- `pg_tx_begin` / `pg_tx_add` / `pg_tx_commit` run several statements atomically.

```pawn
#include <open.mp>
#include <omp_postgres>

new PgConn:g_DB;

public OnGameModeInit()
{
    g_DB = pg_connect("host=127.0.0.1 dbname=game user=game");   // or "" to use PG* env vars / ~/.pgpass
    return 1;
}

LoadAccount(playerid, const name[])
{
    new PgQuery:q = pg_new_query(g_DB, "SELECT id, money FROM accounts WHERE lower(name) = lower($1)");
    pg_bind_str(q, name);
    pg_send(q, "OnAccountLoaded", "d", playerid);
}

forward OnAccountLoaded(playerid);
public OnAccountLoaded(playerid)
{
    if (pg_num_rows() == 0) return; // not registered
    new money = pg_get_int(0, "money");
}
```

The full API, with comments, is in [`include/omp_postgres.inc`](include/omp_postgres.inc).
[`examples/pg_test.pwn`](examples/pg_test.pwn) is a filterscript that exercises it end to end.

## Install

Download the archive for your platform from the [releases](../../releases) and extract it into
the server directory:

- **Linux** (`omp-postgres-<version>-linux.tar.gz`): `components/Postgres.so` and
  `qawno/include/omp_postgres.inc`. The server needs the **32-bit** libpq at runtime, because
  omp-server is a 32-bit program: `libpq.i686` (Fedora) or `libpq5:i386` (Debian/Ubuntu).
- **Windows** (`omp-postgres-<version>-windows.zip`): `components/Postgres.dll` and
  `qawno/include/omp_postgres.inc`. libpq is linked in statically, so nothing else is needed;
  its license and those of its dependencies (OpenSSL, zlib, lz4) are in `licenses/`.

## Connecting

`pg_connect` takes any libpq connection string. `pg_connect("")` uses libpq's standard
`PGHOST`, `PGPORT`, `PGDATABASE`, `PGUSER`, `PGPASSWORD` environment variables, `~/.pgpass`
and `PGSERVICE`, which keeps credentials out of scripts.

Defaults that the connection string (or `PGCLIENTENCODING` / `PGCONNECT_TIMEOUT`) can override:
`client_encoding=WIN1252`, `connect_timeout=10`, `application_name=omp-postgres`, and
`keepalives_idle=30 keepalives_interval=10 keepalives_count=3 tcp_user_timeout=30000`, so a
vanished server or network is noticed within about a minute instead of after 15+ minutes.

A connection belongs to the script that opened it and is closed when that script unloads
(after its queued queries have run), so a GMX doesn't touch filterscript connections.
Handles are plain numbers, so another script can use a connection passed to it, but the
connection still closes when its owner unloads.

**Closing** (`pg_close`, script unload, GMX, shutdown) waits for the queued queries, since those
are usually saves. So that a query stuck on a row lock can't freeze the server, a query still
running after 10 seconds is cancelled (`57014`) and the queries queued behind it fail without
running (`08003`).

**Encoding:** with `WIN1252`, text that has no Windows-1252 equivalent (emoji written by a web
panel, for example) makes the query that reads it fail with `22P05`. Use `client_encoding=UTF8`
if the database holds such text and the script handles UTF-8.

**Database on another machine:** add `sslmode=verify-full sslrootcert=...` to the connection
string. libpq's default (`sslmode=prefer`) encrypts when it can but does not check who it is
talking to, and silently falls back to plain text.

**Transactions:** use `pg_tx_begin` / `pg_tx_add` / `pg_tx_commit`, which send `BEGIN`, the
statements and `COMMIT` as one unit and roll back if any of them fails. Don't send `BEGIN` and
`COMMIT` as separate queries: a dropped connection is re-established automatically, and an open
transaction does not survive that.

**Lost connections:** a connection the server closed while idle (restart, `pg_terminate_backend`)
is noticed before the next query and re-established, so that query runs normally. If the
connection drops while a query runs:

| SQLSTATE | Meaning |
|----------|---------|
| `08006` | Not applied: the connection was already down, or a transaction was rolled back. Transactions that drop before `COMMIT` are retried once automatically, so you only see this if that failed too. |
| `08007` | Unknown: a single statement or a `COMMIT` was sent and the connection dropped before the reply. Check before retrying a write. |
| `57P01` and other server codes | The server ended the statement, so it was not applied. |

**PgBouncer** in transaction pooling mode (before 1.21) does not support prepared statements;
call `pg_set_statement_cache(conn, 0)` after connecting.

## Build

The component must be 32-bit. On Fedora:

```sh
sudo dnf install gcc-c++ cmake ninja-build glibc-devel.i686 libstdc++-devel.i686 \
  libstdc++-static.i686 libatomic.i686 libpq-devel.i686
./build.sh                    # -> build/Postgres.so
./build.sh /path/to/server    # build and install into a server
./package.sh                  # -> dist/omp-postgres-<version>-linux.tar.gz
```

On Debian/Ubuntu: `dpkg --add-architecture i386 && apt install g++-multilib cmake ninja-build libpq-dev:i386`.

On Windows, with Visual Studio 2022 and [vcpkg](https://vcpkg.io). CMake installs libpq through
vcpkg while configuring, at the versions pinned by `builtin-baseline` in `vcpkg.json`:

```bat
cmake -S . -B build -A Win32 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x86-windows-static
cmake --build build --config Release
```

This produces `build\Release\Postgres.dll`.

The open.mp SDK and the Pawn AMX headers are fetched by CMake at pinned commits.
The version is set once, in `project()` in `CMakeLists.txt`.

### Tests

`tests/connection_test.cpp` tests the connection layer (reconnects, retries, the statement cache,
transactions, closing) against a real server, found through the usual `PG*` environment variables.
It creates and drops tables named `omp_test_*`, so point it at a scratch database:

```sh
cmake -S . -B build -DOMP_POSTGRES_BUILD_TESTS=ON && cmake --build build
PGHOST=127.0.0.1 PGUSER=postgres PGDATABASE=scratch ctest --test-dir build --output-on-failure
```

CI builds both platforms and runs the tests on every push; pushing a `v*` tag publishes a release
with both archives.

## License

MIT, see [LICENSE](LICENSE).
