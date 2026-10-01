// omp-postgres: asynchronous PostgreSQL access for open.mp.
//
// All queries run on a background thread per connection, in the order they
// were sent. When a query finishes, its callback runs on the main thread and
// the result is readable with the pg_get_* natives only inside that callback.
//
// Parameters ($1, $2, ...) are sent separately from the SQL text, so values
// never need escaping and SQL injection is impossible:
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

// ---- Connections ------------------------------------------------------------

// Connects synchronously (call it from OnGameModeInit). `conninfo` is a libpq
// connection string, e.g. "host=127.0.0.1 dbname=coffee user=coffee password=...".
// Returns PG_INVALID_CONNECTION on failure; see pg_connect_error.
// The client encoding defaults to WIN1252 (what SA-MP clients send).
native PgConn:pg_connect(const conninfo[]);
native pg_connect_error(dest[], size = sizeof dest);

// Waits for queued queries to finish, then closes the connection.
native bool:pg_close(PgConn:conn);
native bool:pg_is_connected(PgConn:conn);

// Queries waiting or running on this connection, or -1 for an invalid handle.
native pg_pending(PgConn:conn);

// ---- Queries ----------------------------------------------------------------

// Starts building a parameterised query. Bind values in order: the first
// pg_bind_* call fills $1, the second $2, and so on. Then pg_send it (or
// pg_discard it); the handle is invalid afterwards.
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

// ---- Results (only inside a query callback) ---------------------------------

native pg_num_rows();
native pg_num_fields();
// Rows changed by INSERT/UPDATE/DELETE.
native pg_affected_rows();
native pg_field_name(field, dest[], size = sizeof dest);

// Field names are case-sensitive and match what the query returns.
// NULL values (and errors) yield the fallback.
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
// Postgres error code (e.g. "23505" = unique violation, "08006" = connection lost).
forward OnPgError(PgConn:conn, const sqlstate[], const error[], const callback[], const query[]);
