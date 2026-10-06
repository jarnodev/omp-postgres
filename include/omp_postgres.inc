// omp-postgres: asynchronous PostgreSQL access for open.mp.
//
// All queries run on a background thread per connection, in the order they
// were sent. When a query finishes, its callback runs on the main thread and
// the result is readable with the pg_get_* natives only inside that callback.
//
// Parameters ($1, $2, ...) are sent separately from the SQL text, so bound
// values never need escaping and can't inject SQL:
//
//     new PgQuery:q = pg_new_query(g_DB, "SELECT id, money FROM accounts WHERE name = $1");
//     pg_bind_str(q, name);
//     pg_send(q, "OnAccountLoaded", "d", playerid);
//
//     forward OnAccountLoaded(playerid);
//     public OnAccountLoaded(playerid)
//     {
//         if (pg_num_rows() == 0) return; // not registered
//         new money = pg_get_int(0, "money");
//     }
//
// Failed queries do NOT run their callback; OnPgError is called instead.

#if defined _omp_postgres_included
	#endinput
#endif
#define _omp_postgres_included

#define PG_INVALID_CONNECTION (PgConn:0)
#define PG_INVALID_QUERY      (PgQuery:0)
#define PG_INVALID_TX         (PgTx:0)

// ---- Connections ------------------------------------------------------------

// Connects synchronously (call it from OnGameModeInit). `conninfo` is a libpq
// connection string, e.g. "host=127.0.0.1 dbname=coffee user=coffee password=...".
// Returns PG_INVALID_CONNECTION on failure; see pg_connect_error.
// The client encoding defaults to WIN1252 (what SA-MP clients send).
native PgConn:pg_connect(const conninfo[]);
native pg_connect_error(dest[], size = sizeof dest);

// Waits for queued queries to finish, then closes the connection. A query still
// running after 10 seconds is cancelled and the ones behind it fail (SQLSTATE
// "08003"), so a lock wait can't freeze the server.
native bool:pg_close(PgConn:conn);
// The state seen by the last query: a connection that dropped while idle reports
// true until the next query notices (and reconnects).
native bool:pg_is_connected(PgConn:conn);

// Queries waiting or running on this connection, or -1 for an invalid handle.
native pg_pending(PgConn:conn);

// ---- Queries ----------------------------------------------------------------

// Starts building a parameterised query. Bind values in order: the first
// pg_bind_* call fills $1, the second $2, and so on. Then pg_send it, add it
// to a transaction with pg_tx_add, or pg_discard it; the handle is invalid afterwards.
// The statement is prepared once per connection and reused, so keep the SQL
// fixed and put everything that varies in parameters.
native PgQuery:pg_new_query(PgConn:conn, const sql[]);
native bool:pg_bind_int(PgQuery:query, value);
native bool:pg_bind_float(PgQuery:query, Float:value);
native bool:pg_bind_str(PgQuery:query, const value[]);
native bool:pg_bind_bool(PgQuery:query, bool:value);
native bool:pg_bind_null(PgQuery:query);

// `format` describes the extra callback arguments: d/i = integer, f = float,
// b = bool, s = string.
native bool:pg_send(PgQuery:query, const callback[] = "", const format[] = "", {Float, _}:...);
native bool:pg_discard(PgQuery:query);

// Shortcut for SQL without parameters. Never format player input into `sql`;
// use pg_new_query + pg_bind_* for that.
native bool:pg_query(PgConn:conn, const sql[], const callback[] = "", const format[] = "", {Float, _}:...);

// How many prepared statements a connection keeps (least recently used are
// dropped; default 100). 0 turns preparing off, which is needed behind
// PgBouncer in transaction pooling mode.
native bool:pg_set_statement_cache(PgConn:conn, size);

// ---- Transactions -----------------------------------------------------------

// Runs several queries atomically: all of them are applied, or none.
//
//     new PgTx:tx = pg_tx_begin(g_DB);
//     new PgQuery:q = pg_new_query(g_DB, "UPDATE accounts SET money = money - $1 WHERE id = $2");
//     pg_bind_int(q, amount); pg_bind_int(q, fromId);
//     pg_tx_add(tx, q);
//     q = pg_new_query(g_DB, "UPDATE accounts SET money = money + $1 WHERE id = $2");
//     pg_bind_int(q, amount); pg_bind_int(q, toId);
//     pg_tx_add(tx, q);
//     pg_tx_commit(tx, "OnTransferDone", "d", playerid);
//
// If any statement fails, the transaction is rolled back and OnPgError gets
// that statement's SQL. SQLSTATE "08007" means the connection dropped during
// COMMIT, so whether it was applied is unknown.
// In the callback, pg_select_result(i) chooses which statement's result the
// pg_get_* natives read; the last one is selected by default.
native PgTx:pg_tx_begin(PgConn:conn);
// Moves a query built with pg_new_query into the transaction (the query handle is invalid afterwards).
native bool:pg_tx_add(PgTx:tx, PgQuery:query);
native bool:pg_tx_commit(PgTx:tx, const callback[] = "", const format[] = "", {Float, _}:...);
native bool:pg_tx_discard(PgTx:tx);

// ---- Results (only inside a query callback) ---------------------------------

// Number of results: 1 for a query, one per statement for a transaction.
native pg_result_count();
native bool:pg_select_result(index);

native pg_num_rows();
native pg_num_fields();
// Rows changed by INSERT/UPDATE/DELETE.
native pg_affected_rows();
native pg_field_name(field, dest[], size = sizeof dest);

// Field names are case-sensitive and match what the query returns.
// NULL values (and errors) yield the fallback. pg_get_int truncates decimals
// ("12.5" -> 12) and clamps values outside the 32-bit range.
native bool:pg_is_null(row, const field[]);
native pg_get_int(row, const field[], fallback = 0);
native Float:pg_get_float(row, const field[], Float:fallback = 0.0);
native bool:pg_get_bool(row, const field[], bool:fallback = false);
// Returns the string length, 0 for NULL, -1 on error.
native pg_get_str(row, const field[], dest[], size = sizeof dest);

// Same, addressing the field by column index (0-based).
native bool:pg_is_null_idx(row, field);
native pg_get_int_idx(row, field, fallback = 0);
native Float:pg_get_float_idx(row, field, Float:fallback = 0.0);
native bool:pg_get_bool_idx(row, field, bool:fallback = false);
native pg_get_str_idx(row, field, dest[], size = sizeof dest);

// ---- Callbacks --------------------------------------------------------------

// Called in the script that sent a query when it fails. `sqlstate` is the
// Postgres error code, e.g. "23505" = unique violation, "08006" = connection lost
// and the query was not applied, "08007" = connection lost after sending, so it
// may or may not have been applied. Return 0 to keep the error out of the server
// log (when the script has handled it), 1 to log it as well.
forward OnPgError(PgConn:conn, const sqlstate[], const error[], const callback[], const query[]);
