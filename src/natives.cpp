// Pawn natives. Keep in sync with include/omp_postgres.inc.
#include "component.hpp"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace
{
std::string g_lastConnectError;

PostgresComponent& component()
{
	return *PostgresComponent::instance();
}

int paramCount(const cell* params)
{
	return static_cast<int>(params[0] / sizeof(cell));
}

float cellToFloat(cell c)
{
	float f;
	std::memcpy(&f, &c, sizeof f);
	return f;
}

cell floatToCell(float f)
{
	cell c;
	std::memcpy(&c, &f, sizeof c);
	return c;
}

std::string readString(IPawnScript& script, cell addr)
{
	cell* phys = nullptr;
	if (script.GetAddr(addr, &phys) != AMX_ERR_NONE || phys == nullptr)
	{
		return {};
	}
	int len = 0;
	script.StrLen(phys, &len);
	std::vector<char> buf(len + 1);
	script.GetString(buf.data(), phys, false, buf.size());
	return std::string(buf.data(), len);
}

bool readCell(IPawnScript& script, cell addr, cell& out)
{
	cell* phys = nullptr;
	if (script.GetAddr(addr, &phys) != AMX_ERR_NONE || phys == nullptr)
	{
		return false;
	}
	out = *phys;
	return true;
}

void writeString(IPawnScript& script, cell addr, StringView str, cell size)
{
	cell* phys = nullptr;
	if (size <= 0 || script.GetAddr(addr, &phys) != AMX_ERR_NONE || phys == nullptr)
	{
		return;
	}
	script.SetString(phys, str, false, false, static_cast<size_t>(size));
}

#define PG_NATIVE(name) cell AMX_NATIVE_CALL name(AMX* amx, const cell* params)

#define PG_REQUIRE_PARAMS(name, n)                                                              \
	IPawnScript* script = component().scriptFor(amx);                                           \
	if (script == nullptr)                                                                      \
	{                                                                                           \
		return 0;                                                                               \
	}                                                                                           \
	if (paramCount(params) < (n))                                                               \
	{                                                                                           \
		component().logError("[postgres] %s: expected %d parameters, got %d.", name, (n), paramCount(params)); \
		return 0;                                                                               \
	}

// Reads the callback name, format string and variadic arguments starting at params[first].
// Variadic Pawn arguments are always passed by reference.
bool readCallback(IPawnScript& script, const cell* params, int first, const char* native, Query& query)
{
	int count = paramCount(params);
	query.script = &script;
	query.scriptGeneration = component().generationOf(&script);
	if (count < first)
	{
		return true;
	}
	query.callback = readString(script, params[first]);
	if (count < first + 1)
	{
		return true;
	}
	std::string format = readString(script, params[first + 1]);
	int argIndex = first + 2;
	if (count - argIndex + 1 < static_cast<int>(format.size()))
	{
		component().logError("[postgres] %s: format \"%s\" expects %zu arguments, got %d.", native, format.c_str(),
			format.size(), count - argIndex + 1);
		return false;
	}
	for (char spec : format)
	{
		CallbackArg arg;
		switch (spec)
		{
		case 'd':
		case 'i':
		case 'f':
		case 'b':
			readCell(script, params[argIndex], arg.cell);
			break;
		case 's':
			arg.isString = true;
			arg.str = readString(script, params[argIndex]);
			break;
		default:
			component().logError("[postgres] %s: unknown format specifier '%c' (use d, i, f, b or s).", native, spec);
			return false;
		}
		query.args.push_back(std::move(arg));
		++argIndex;
	}
	return true;
}

// ---- Connections -----------------------------------------------------------

// PgConn:pg_connect(const conninfo[])
PG_NATIVE(n_pg_connect)
{
	PG_REQUIRE_PARAMS("pg_connect", 1);
	g_lastConnectError.clear();
	int id = component().connect(script, readString(*script, params[1]), g_lastConnectError);
	if (id == 0)
	{
		component().logError("[postgres] Connection failed: %s", g_lastConnectError.c_str());
	}
	return id;
}

// pg_connect_error(dest[], size = sizeof dest)
PG_NATIVE(n_pg_connect_error)
{
	PG_REQUIRE_PARAMS("pg_connect_error", 2);
	writeString(*script, params[1], StringView(g_lastConnectError), params[2]);
	return static_cast<cell>(g_lastConnectError.size());
}

// bool:pg_close(PgConn:conn)
PG_NATIVE(n_pg_close)
{
	PG_REQUIRE_PARAMS("pg_close", 1);
	return component().disconnect(params[1]);
}

// bool:pg_is_connected(PgConn:conn)
PG_NATIVE(n_pg_is_connected)
{
	PG_REQUIRE_PARAMS("pg_is_connected", 1);
	Connection* conn = component().connection(params[1]);
	return conn && conn->isConnected();
}

// pg_pending(PgConn:conn)
PG_NATIVE(n_pg_pending)
{
	PG_REQUIRE_PARAMS("pg_pending", 1);
	Connection* conn = component().connection(params[1]);
	return conn ? static_cast<cell>(conn->pending()) : -1;
}

// ---- Queries ---------------------------------------------------------------

// PgQuery:pg_new_query(PgConn:conn, const sql[])
PG_NATIVE(n_pg_new_query)
{
	PG_REQUIRE_PARAMS("pg_new_query", 2);
	if (component().connection(params[1]) == nullptr)
	{
		component().logError("[postgres] pg_new_query: invalid connection %d.", params[1]);
		return 0;
	}
	return component().newPendingQuery(params[1], script, readString(*script, params[2]));
}

PendingQuery* pendingFor(cell id, const char* native)
{
	PendingQuery* q = component().pendingQuery(id);
	if (q == nullptr)
	{
		component().logError("[postgres] %s: invalid query handle %d (already sent or discarded?).", native, id);
	}
	return q;
}

bool bindValue(cell id, const char* native, QueryParam param)
{
	PendingQuery* q = pendingFor(id, native);
	if (q == nullptr)
	{
		return false;
	}
	q->statement.params.push_back(std::move(param));
	return true;
}

// bool:pg_bind_int(PgQuery:query, value)
PG_NATIVE(n_pg_bind_int)
{
	PG_REQUIRE_PARAMS("pg_bind_int", 2);
	return bindValue(params[1], "pg_bind_int", { false, std::to_string(params[2]) });
}

// bool:pg_bind_float(PgQuery:query, Float:value)
PG_NATIVE(n_pg_bind_float)
{
	PG_REQUIRE_PARAMS("pg_bind_float", 2);
	char buf[32];
	std::snprintf(buf, sizeof buf, "%.9g", cellToFloat(params[2]));
	return bindValue(params[1], "pg_bind_float", { false, buf });
}

// bool:pg_bind_str(PgQuery:query, const value[])
PG_NATIVE(n_pg_bind_str)
{
	PG_REQUIRE_PARAMS("pg_bind_str", 2);
	return bindValue(params[1], "pg_bind_str", { false, readString(*script, params[2]) });
}

// bool:pg_bind_bool(PgQuery:query, bool:value)
PG_NATIVE(n_pg_bind_bool)
{
	PG_REQUIRE_PARAMS("pg_bind_bool", 2);
	return bindValue(params[1], "pg_bind_bool", { false, params[2] ? "true" : "false" });
}

// bool:pg_bind_null(PgQuery:query)
PG_NATIVE(n_pg_bind_null)
{
	PG_REQUIRE_PARAMS("pg_bind_null", 1);
	return bindValue(params[1], "pg_bind_null", { true, {} });
}

// bool:pg_send(PgQuery:query, const callback[] = "", const format[] = "", {Float, _}:...)
PG_NATIVE(n_pg_send)
{
	PG_REQUIRE_PARAMS("pg_send", 1);
	PendingQuery* pending = pendingFor(params[1], "pg_send");
	if (pending == nullptr)
	{
		return false;
	}
	auto query = std::make_unique<Query>();
	query->connectionId = pending->connectionId;
	query->statements.push_back(std::move(pending->statement));
	component().dropPendingQuery(params[1]);

	if (!readCallback(*script, params, 2, "pg_send", *query))
	{
		return false;
	}
	if (component().connection(query->connectionId) == nullptr)
	{
		component().logError("[postgres] pg_send: connection %d was closed.", query->connectionId);
		return false;
	}
	component().send(std::move(query));
	return true;
}

// bool:pg_discard(PgQuery:query)
PG_NATIVE(n_pg_discard)
{
	PG_REQUIRE_PARAMS("pg_discard", 1);
	bool existed = component().pendingQuery(params[1]) != nullptr;
	component().dropPendingQuery(params[1]);
	return existed;
}

// bool:pg_query(PgConn:conn, const sql[], const callback[] = "", const format[] = "", {Float, _}:...)
PG_NATIVE(n_pg_query)
{
	PG_REQUIRE_PARAMS("pg_query", 2);
	if (component().connection(params[1]) == nullptr)
	{
		component().logError("[postgres] pg_query: invalid connection %d.", params[1]);
		return false;
	}
	auto query = std::make_unique<Query>();
	query->connectionId = params[1];
	query->statements.push_back(Statement { readString(*script, params[2]), {}, false });
	if (!readCallback(*script, params, 3, "pg_query", *query))
	{
		return false;
	}
	component().send(std::move(query));
	return true;
}

// bool:pg_set_statement_cache(PgConn:conn, size)
PG_NATIVE(n_pg_set_statement_cache)
{
	PG_REQUIRE_PARAMS("pg_set_statement_cache", 2);
	Connection* conn = component().connection(params[1]);
	if (conn == nullptr)
	{
		component().logError("[postgres] pg_set_statement_cache: invalid connection %d.", params[1]);
		return false;
	}
	conn->setStatementCacheSize(params[2]);
	return true;
}

// ---- Transactions ----------------------------------------------------------

PendingTransaction* transactionFor(cell id, const char* native)
{
	PendingTransaction* tx = component().pendingTransaction(id);
	if (tx == nullptr)
	{
		component().logError("[postgres] %s: invalid transaction handle %d (already committed or discarded?).", native, id);
	}
	return tx;
}

// PgTx:pg_tx_begin(PgConn:conn)
PG_NATIVE(n_pg_tx_begin)
{
	PG_REQUIRE_PARAMS("pg_tx_begin", 1);
	if (component().connection(params[1]) == nullptr)
	{
		component().logError("[postgres] pg_tx_begin: invalid connection %d.", params[1]);
		return 0;
	}
	return component().newPendingTransaction(params[1], script);
}

// bool:pg_tx_add(PgTx:tx, PgQuery:query)
PG_NATIVE(n_pg_tx_add)
{
	PG_REQUIRE_PARAMS("pg_tx_add", 2);
	PendingTransaction* tx = transactionFor(params[1], "pg_tx_add");
	PendingQuery* query = pendingFor(params[2], "pg_tx_add");
	if (tx == nullptr || query == nullptr)
	{
		return false;
	}
	if (query->connectionId != tx->connectionId)
	{
		component().logError("[postgres] pg_tx_add: query %d is for connection %d, transaction %d for connection %d.",
			params[2], query->connectionId, params[1], tx->connectionId);
		return false;
	}
	tx->statements.push_back(std::move(query->statement));
	component().dropPendingQuery(params[2]);
	return true;
}

// bool:pg_tx_commit(PgTx:tx, const callback[] = "", const format[] = "", {Float, _}:...)
PG_NATIVE(n_pg_tx_commit)
{
	PG_REQUIRE_PARAMS("pg_tx_commit", 1);
	PendingTransaction* tx = transactionFor(params[1], "pg_tx_commit");
	if (tx == nullptr)
	{
		return false;
	}
	auto query = std::make_unique<Query>();
	query->connectionId = tx->connectionId;
	query->transaction = true;
	query->statements = std::move(tx->statements);
	component().dropPendingTransaction(params[1]);

	if (!readCallback(*script, params, 2, "pg_tx_commit", *query))
	{
		return false;
	}
	if (component().connection(query->connectionId) == nullptr)
	{
		component().logError("[postgres] pg_tx_commit: connection %d was closed.", query->connectionId);
		return false;
	}
	component().send(std::move(query));
	return true;
}

// bool:pg_tx_discard(PgTx:tx)
PG_NATIVE(n_pg_tx_discard)
{
	PG_REQUIRE_PARAMS("pg_tx_discard", 1);
	bool existed = component().pendingTransaction(params[1]) != nullptr;
	component().dropPendingTransaction(params[1]);
	return existed;
}

// ---- Results (only valid inside a query callback) ---------------------------

const PGresult* activeResult(const char* native)
{
	const PGresult* res = component().activeResult();
	if (res == nullptr)
	{
		component().logError("[postgres] %s: no active result (only valid inside a query callback).", native);
	}
	return res;
}

// Resolves a row/column to a cell value. Returns nullptr for NULL or out-of-range.
const char* valueAt(const char* native, cell row, int column, bool& isNull)
{
	isNull = true;
	const PGresult* res = activeResult(native);
	// A negative column means columnByName already reported the problem.
	if (res == nullptr || column < 0)
	{
		return nullptr;
	}
	if (row < 0 || row >= PQntuples(res) || column < 0 || column >= PQnfields(res))
	{
		component().logError("[postgres] %s: row %d / column %d out of range (%d rows, %d columns).", native, row,
			column, PQntuples(res), PQnfields(res));
		return nullptr;
	}
	if (PQgetisnull(res, row, column))
	{
		return nullptr;
	}
	isNull = false;
	return PQgetvalue(res, row, column);
}

int columnByName(IPawnScript& script, cell nameAddr, const char* native)
{
	const PGresult* res = component().activeResult();
	if (res == nullptr)
	{
		return -1;
	}
	std::string name = readString(script, nameAddr);
	// Quote it so PQfnumber matches case-sensitively, the way names come back from Postgres.
	int column = PQfnumber(res, ("\"" + name + "\"").c_str());
	if (column < 0)
	{
		component().logError("[postgres] %s: no column named \"%s\" in the result.", native, name.c_str());
	}
	return column;
}

cell parseInt(const char* value, cell fallback)
{
	errno = 0;
	char* end = nullptr;
	long long v = std::strtoll(value, &end, 10);
	if (end == value || errno == ERANGE)
	{
		return fallback;
	}
	if (v > INT32_MAX)
	{
		return INT32_MAX;
	}
	if (v < INT32_MIN)
	{
		return INT32_MIN;
	}
	return static_cast<cell>(v);
}

cell getInt(const char* native, cell row, int column, cell fallback)
{
	bool isNull;
	const char* value = valueAt(native, row, column, isNull);
	return value ? parseInt(value, fallback) : fallback;
}

cell getFloat(const char* native, cell row, int column, cell fallback)
{
	bool isNull;
	const char* value = valueAt(native, row, column, isNull);
	if (value == nullptr)
	{
		return fallback;
	}
	char* end = nullptr;
	float f = std::strtof(value, &end);
	return end == value ? fallback : floatToCell(f);
}

cell getBool(const char* native, cell row, int column, cell fallback)
{
	bool isNull;
	const char* value = valueAt(native, row, column, isNull);
	if (value == nullptr)
	{
		return fallback;
	}
	// Postgres renders booleans as 't'/'f'; accept numbers too.
	if (value[0] == 't' || value[0] == 'T')
	{
		return 1;
	}
	if (value[0] == 'f' || value[0] == 'F')
	{
		return 0;
	}
	return parseInt(value, fallback) != 0;
}

cell getStr(IPawnScript& script, const char* native, cell row, int column, cell dest, cell size)
{
	bool isNull;
	const char* value = valueAt(native, row, column, isNull);
	if (value == nullptr)
	{
		writeString(script, dest, StringView(""), size);
		return isNull ? 0 : -1;
	}
	writeString(script, dest, StringView(value), size);
	return static_cast<cell>(std::strlen(value));
}

// pg_result_count()
PG_NATIVE(n_pg_result_count)
{
	PG_REQUIRE_PARAMS("pg_result_count", 0);
	return component().activeResultCount();
}

// bool:pg_select_result(index)
PG_NATIVE(n_pg_select_result)
{
	PG_REQUIRE_PARAMS("pg_select_result", 1);
	if (!component().selectResult(params[1]))
	{
		component().logError("[postgres] pg_select_result: no result %d (%d available).", params[1],
			component().activeResultCount());
		return false;
	}
	return true;
}

// pg_num_rows()
PG_NATIVE(n_pg_num_rows)
{
	PG_REQUIRE_PARAMS("pg_num_rows", 0);
	const PGresult* res = activeResult("pg_num_rows");
	return res ? PQntuples(res) : 0;
}

// pg_num_fields()
PG_NATIVE(n_pg_num_fields)
{
	PG_REQUIRE_PARAMS("pg_num_fields", 0);
	const PGresult* res = activeResult("pg_num_fields");
	return res ? PQnfields(res) : 0;
}

// pg_affected_rows()
PG_NATIVE(n_pg_affected_rows)
{
	PG_REQUIRE_PARAMS("pg_affected_rows", 0);
	const PGresult* res = activeResult("pg_affected_rows");
	if (res == nullptr)
	{
		return 0;
	}
	return parseInt(PQcmdTuples(const_cast<PGresult*>(res)), 0);
}

// pg_field_name(field, dest[], size = sizeof dest)
PG_NATIVE(n_pg_field_name)
{
	PG_REQUIRE_PARAMS("pg_field_name", 3);
	const PGresult* res = activeResult("pg_field_name");
	const char* name = res ? PQfname(res, params[1]) : nullptr;
	writeString(*script, params[2], StringView(name ? name : ""), params[3]);
	return name != nullptr;
}

// bool:pg_is_null(row, const field[])
PG_NATIVE(n_pg_is_null)
{
	PG_REQUIRE_PARAMS("pg_is_null", 2);
	bool isNull;
	valueAt("pg_is_null", params[1], columnByName(*script, params[2], "pg_is_null"), isNull);
	return isNull;
}

// bool:pg_is_null_idx(row, field)
PG_NATIVE(n_pg_is_null_idx)
{
	PG_REQUIRE_PARAMS("pg_is_null_idx", 2);
	bool isNull;
	valueAt("pg_is_null_idx", params[1], params[2], isNull);
	return isNull;
}

// pg_get_int(row, const field[], fallback = 0)
PG_NATIVE(n_pg_get_int)
{
	PG_REQUIRE_PARAMS("pg_get_int", 3);
	return getInt("pg_get_int", params[1], columnByName(*script, params[2], "pg_get_int"), params[3]);
}

// pg_get_int_idx(row, field, fallback = 0)
PG_NATIVE(n_pg_get_int_idx)
{
	PG_REQUIRE_PARAMS("pg_get_int_idx", 3);
	return getInt("pg_get_int_idx", params[1], params[2], params[3]);
}

// Float:pg_get_float(row, const field[], Float:fallback = 0.0)
PG_NATIVE(n_pg_get_float)
{
	PG_REQUIRE_PARAMS("pg_get_float", 3);
	return getFloat("pg_get_float", params[1], columnByName(*script, params[2], "pg_get_float"), params[3]);
}

// Float:pg_get_float_idx(row, field, Float:fallback = 0.0)
PG_NATIVE(n_pg_get_float_idx)
{
	PG_REQUIRE_PARAMS("pg_get_float_idx", 3);
	return getFloat("pg_get_float_idx", params[1], params[2], params[3]);
}

// bool:pg_get_bool(row, const field[], bool:fallback = false)
PG_NATIVE(n_pg_get_bool)
{
	PG_REQUIRE_PARAMS("pg_get_bool", 3);
	return getBool("pg_get_bool", params[1], columnByName(*script, params[2], "pg_get_bool"), params[3]);
}

// bool:pg_get_bool_idx(row, field, bool:fallback = false)
PG_NATIVE(n_pg_get_bool_idx)
{
	PG_REQUIRE_PARAMS("pg_get_bool_idx", 3);
	return getBool("pg_get_bool_idx", params[1], params[2], params[3]);
}

// pg_get_str(row, const field[], dest[], size = sizeof dest)
PG_NATIVE(n_pg_get_str)
{
	PG_REQUIRE_PARAMS("pg_get_str", 4);
	return getStr(*script, "pg_get_str", params[1], columnByName(*script, params[2], "pg_get_str"), params[3], params[4]);
}

// pg_get_str_idx(row, field, dest[], size = sizeof dest)
PG_NATIVE(n_pg_get_str_idx)
{
	PG_REQUIRE_PARAMS("pg_get_str_idx", 4);
	return getStr(*script, "pg_get_str_idx", params[1], params[2], params[3], params[4]);
}

const AMX_NATIVE_INFO g_natives[] = {
	{ "pg_connect", n_pg_connect },
	{ "pg_connect_error", n_pg_connect_error },
	{ "pg_close", n_pg_close },
	{ "pg_is_connected", n_pg_is_connected },
	{ "pg_pending", n_pg_pending },

	{ "pg_new_query", n_pg_new_query },
	{ "pg_bind_int", n_pg_bind_int },
	{ "pg_bind_float", n_pg_bind_float },
	{ "pg_bind_str", n_pg_bind_str },
	{ "pg_bind_bool", n_pg_bind_bool },
	{ "pg_bind_null", n_pg_bind_null },
	{ "pg_send", n_pg_send },
	{ "pg_discard", n_pg_discard },
	{ "pg_query", n_pg_query },
	{ "pg_set_statement_cache", n_pg_set_statement_cache },

	{ "pg_tx_begin", n_pg_tx_begin },
	{ "pg_tx_add", n_pg_tx_add },
	{ "pg_tx_commit", n_pg_tx_commit },
	{ "pg_tx_discard", n_pg_tx_discard },

	{ "pg_result_count", n_pg_result_count },
	{ "pg_select_result", n_pg_select_result },

	{ "pg_num_rows", n_pg_num_rows },
	{ "pg_num_fields", n_pg_num_fields },
	{ "pg_affected_rows", n_pg_affected_rows },
	{ "pg_field_name", n_pg_field_name },
	{ "pg_is_null", n_pg_is_null },
	{ "pg_is_null_idx", n_pg_is_null_idx },
	{ "pg_get_int", n_pg_get_int },
	{ "pg_get_int_idx", n_pg_get_int_idx },
	{ "pg_get_float", n_pg_get_float },
	{ "pg_get_float_idx", n_pg_get_float_idx },
	{ "pg_get_bool", n_pg_get_bool },
	{ "pg_get_bool_idx", n_pg_get_bool_idx },
	{ "pg_get_str", n_pg_get_str },
	{ "pg_get_str_idx", n_pg_get_str_idx },
};
}

void registerNatives(IPawnScript& script)
{
	script.Register(g_natives, static_cast<int>(sizeof(g_natives) / sizeof(g_natives[0])));
}
