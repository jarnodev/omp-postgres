# Changelog

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
