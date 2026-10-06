# Changelog

## 0.3.0

### Added
- `OnPgError` can return 0 to keep a handled error out of the server log.
- A warning in the log when many queries built with `pg_new_query` are never sent or discarded,
  naming the most common SQL.
- Connection tests (`tests/`), run by CI against PostgreSQL 18.
- The Windows archive includes the licenses of the statically linked libpq, OpenSSL, zlib and lz4.

### Changed
- Closing a connection (`pg_close`, script unload, GMX, shutdown) cancels a query still running
  after 10 seconds and fails the ones queued behind it with `08003`, instead of waiting forever.
- Connections default to TCP keepalives and `tcp_user_timeout=30000`, so a vanished server or
  network is noticed within about a minute.
- A single statement whose connection dropped after it was sent fails with `08007` (outcome
  unknown) instead of `08006`.
- Windows builds pin libpq through `vcpkg.json`; CMake installs it while configuring.

### Fixed
- The first query after the server closed an idle connection (restart, `pg_terminate_backend`)
  failed; the drop is now noticed and the connection re-established before sending.
- A transaction whose connection dropped before `COMMIT` failed instead of being retried.
- Handle ids could overflow after 2^31 queries and return the invalid handle 0.
- `pg_get_str`, `pg_field_name` and `pg_connect_error` could write past the script's memory when
  given a size larger than the array.
- `pg_bind_float` and `pg_get_float` depended on the C locale.
- `pg_get_int` returned the fallback for values beyond 64 bits instead of clamping like other
  out-of-range values.
- `pg_pending` could still count a query whose callback was running.
- One failing `DEALLOCATE` skipped the rest, leaving statements on the server.
- Any `0A000` error was retried, not just a stale cached statement.
- If pushing callback arguments failed, they were left on the script's stack.
- The Linux build still exported three `std::thread` typeinfo symbols.

### Upgrading
- `OnPgError` used to ignore its return value. Return 1 to keep errors in the server log;
  `return 0` now hides them.
- Treat `08007` like a possibly applied write: check before retrying it.
- Building on Windows: drop the `vcpkg install libpq` step; CMake installs libpq from `vcpkg.json`.
  CMake 3.25 or newer is required.

## 0.2.0

### Added
- Transactions: `pg_tx_begin`, `pg_tx_add`, `pg_tx_commit`, `pg_tx_discard`. `BEGIN`, the
  statements and `COMMIT` run as one unit and roll back if any statement fails. SQLSTATE `08007`
  means the connection dropped during `COMMIT`.
- `pg_result_count` and `pg_select_result` to read each statement's result in a transaction callback.
- Statements built with `pg_new_query` are prepared once per connection and reused (up to 100,
  least recently used are dropped). `pg_set_statement_cache(conn, size)` changes the limit;
  `0` turns it off.
- Windows build (`omp-postgres-<version>-windows.zip`, libpq linked statically).

### Changed
- A connection belongs to the script that opened it and is closed when that script unloads.
  A GMX no longer closes filterscript connections.
- Connections default to `connect_timeout=10` and `application_name=omp-postgres`.

### Fixed
- A query sent while the database was unreachable could block for minutes, and shutdown or
  GMX could hang while queued queries each waited for a reconnect. Reconnects now back off
  for 5 seconds after a failure.
- After a reconnect the client encoding was always reset to `WIN1252`, ignoring
  `client_encoding` in the connection string.
- The Linux build exported its statically linked libstdc++, which could clash with other components.

### Upgrading
- Behind PgBouncer in transaction pooling mode (before 1.21), call
  `pg_set_statement_cache(conn, 0)` after connecting.
- Keep `pg_new_query` SQL fixed and bind everything that varies; SQL with values pasted in
  still works but fills the statement cache.

## 0.1.0

First release.
