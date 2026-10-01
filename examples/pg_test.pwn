// Smoke test for the Postgres component. Load with: loadfs pg_test
// An empty conninfo makes libpq use PGHOST/PGDATABASE/PGUSER/PGPASSWORD,
// ~/.pgpass and PGSERVICE, so credentials stay out of the script.
#define FILTERSCRIPT

#include <open.mp>
#include <omp_postgres>

new PgConn:g_DB = PG_INVALID_CONNECTION;

public OnFilterScriptInit()
{
	g_DB = pg_connect("");
	if (g_DB == PG_INVALID_CONNECTION)
	{
		new error[256];
		pg_connect_error(error);
		printf("[pg_test] FAILED to connect: %s", error);
		return 1;
	}
	print("[pg_test] Connected.");

	pg_query(g_DB, "SELECT version() AS version, now()::text AS now", "OnVersion");

	// Parameters, callback arguments and NULL/bool handling in one go.
	new PgQuery:q = pg_new_query(g_DB, "SELECT $1::int + 1 AS answer, $2::text AS echo, $3::text AS nothing, true AS yes");
	pg_bind_int(q, 41);
	pg_bind_str(q, "Robert'); DROP TABLE accounts;--");
	pg_bind_null(q);
	pg_send(q, "OnParams", "ds", 7, "coffee");

	// Should fail and land in OnPgError.
	pg_query(g_DB, "SELECT * FROM table_that_does_not_exist", "OnNeverCalled");

	// The same prepared statement twice: the second run reuses it.
	for (new i = 0; i < 2; i++)
	{
		q = pg_new_query(g_DB, "SELECT $1::int * 2 AS doubled");
		pg_bind_int(q, 21 + i);
		pg_send(q, "OnPrepared", "d", i);
	}

	// Transaction with two results.
	new PgTx:tx = pg_tx_begin(g_DB);
	q = pg_new_query(g_DB, "SELECT 1 AS first");
	pg_tx_add(tx, q);
	q = pg_new_query(g_DB, "SELECT $1::text AS second");
	pg_bind_str(q, "two");
	pg_tx_add(tx, q);
	pg_tx_commit(tx, "OnTransaction");

	// A failing statement rolls the whole transaction back and lands in OnPgError.
	tx = pg_tx_begin(g_DB);
	pg_tx_add(tx, pg_new_query(g_DB, "SELECT 1"));
	pg_tx_add(tx, pg_new_query(g_DB, "SELECT 1/0"));
	pg_tx_commit(tx, "OnNeverCalled");
	return 1;
}

public OnFilterScriptExit()
{
	if (g_DB != PG_INVALID_CONNECTION)
	{
		pg_close(g_DB);
	}
	return 1;
}

forward OnVersion();
public OnVersion()
{
	new version[128], now[64];
	pg_get_str(0, "version", version);
	pg_get_str(0, "now", now);
	printf("[pg_test] %s @ %s", version, now);
}

forward OnParams(number, const text[]);
public OnParams(number, const text[])
{
	new echo[64];
	pg_get_str(0, "echo", echo);
	printf("[pg_test] answer=%d echo=%s null=%d yes=%d | callback args: %d, %s",
		pg_get_int(0, "answer"), echo, pg_is_null(0, "nothing"), pg_get_bool(0, "yes"), number, text);
}

forward OnPrepared(run);
public OnPrepared(run)
{
	printf("[pg_test] prepared run %d: doubled=%d", run, pg_get_int(0, "doubled"));
}

forward OnTransaction();
public OnTransaction()
{
	new second[16];
	pg_get_str(0, "second", second);
	pg_select_result(0);
	printf("[pg_test] transaction: %d results, first=%d second=%s", pg_result_count(), pg_get_int(0, "first"), second);
}

forward OnNeverCalled();
public OnNeverCalled()
{
	print("[pg_test] ERROR: callback of a failed query was called.");
}

public OnPgError(PgConn:conn, const sqlstate[], const error[], const callback[], const query[])
{
	printf("[pg_test] OnPgError OK (expected): %s %s", sqlstate, error);
	return 1;
}
