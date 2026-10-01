#include "connection.hpp"

#include <cstdlib>

namespace
{
constexpr std::chrono::seconds ReconnectBackoff { 5 };

bool succeeded(const PGresult* res)
{
	ExecStatusType status = PQresultStatus(res);
	return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
}

// Errors raised before the statement ran, so running it again is safe:
// 26000 = the prepared statement is gone (DISCARD ALL, pooler),
// 0A000 = "cached plan must not change result type" after a schema change.
bool retryable(const std::string& sqlState)
{
	return sqlState == "26000" || sqlState == "0A000";
}
}

void CompletionQueue::push(QueryPtr query)
{
	std::lock_guard<std::mutex> lock(mutex_);
	done_.push_back(std::move(query));
}

std::vector<QueryPtr> CompletionQueue::takeAll()
{
	std::vector<QueryPtr> out;
	std::lock_guard<std::mutex> lock(mutex_);
	out.swap(done_);
	return out;
}

PGconn* Connection::open(const std::string& conninfo, std::string& error)
{
	// Later entries win, and `dbname` is expanded into whatever `conninfo` sets.
	// SA-MP clients send Windows-1252 bytes, not UTF-8, so let the server convert
	// unless the conninfo or PGCLIENTENCODING already chose an encoding.
	const char* keys[5];
	const char* values[5];
	int n = 0;
	if (std::getenv("PGCLIENTENCODING") == nullptr)
	{
		keys[n] = "client_encoding";
		values[n++] = "WIN1252";
	}
	// libpq waits forever by default, which would hang pg_connect and every reconnect.
	if (std::getenv("PGCONNECT_TIMEOUT") == nullptr)
	{
		keys[n] = "connect_timeout";
		values[n++] = "10";
	}
	keys[n] = "fallback_application_name";
	values[n++] = "omp-postgres";
	keys[n] = "dbname";
	values[n++] = conninfo.c_str();
	keys[n] = nullptr;
	values[n] = nullptr;

	PGconn* conn = PQconnectdbParams(keys, values, 1);
	if (conn == nullptr)
	{
		error = "out of memory";
		return nullptr;
	}
	if (PQstatus(conn) != CONNECTION_OK)
	{
		error = PQerrorMessage(conn);
		PQfinish(conn);
		return nullptr;
	}
	return conn;
}

Connection::Connection(int id, PGconn* conn, CompletionQueue& completions)
	: id_(id)
	, conn_(conn)
	, completions_(completions)
{
	worker_ = std::thread(&Connection::run, this);
}

Connection::~Connection()
{
	close();
	if (conn_)
	{
		PQfinish(conn_);
		conn_ = nullptr;
	}
}

void Connection::enqueue(QueryPtr query)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.push_back(std::move(query));
	}
	wake_.notify_one();
}

void Connection::close()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_ && !worker_.joinable())
		{
			return;
		}
		stopping_ = true;
	}
	wake_.notify_one();
	if (worker_.joinable())
	{
		worker_.join();
	}
}

size_t Connection::pending() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return queue_.size() + inFlight_.load();
}

void Connection::run()
{
	for (;;)
	{
		QueryPtr query;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
			// Drain the queue before stopping, so saves sent during shutdown still land.
			if (queue_.empty())
			{
				return;
			}
			query = std::move(queue_.front());
			queue_.pop_front();
			inFlight_ = 1;
		}
		execute(*query);
		completions_.push(std::move(query));
		inFlight_ = 0;
	}
}

bool Connection::ensureConnected(std::string& error)
{
	if (PQstatus(conn_) == CONNECTION_OK)
	{
		return true;
	}
	auto now = std::chrono::steady_clock::now();
	if (now < nextReconnect_)
	{
		error = "not connected (waiting to retry)";
		return false;
	}
	// PQreset reconnects with the original parameters, client_encoding included.
	PQreset(conn_);
	// Prepared statements belonged to the old session.
	clearStatementCache();
	if (PQstatus(conn_) == CONNECTION_OK)
	{
		connected_ = true;
		return true;
	}
	connected_ = false;
	nextReconnect_ = std::chrono::steady_clock::now() + ReconnectBackoff;
	error = PQerrorMessage(conn_);
	return false;
}

void Connection::execute(Query& query)
{
	if (!ensureConnected(query.error))
	{
		query.sqlState = "08006"; // connection_failure
		return;
	}
	trimStatementCache(static_cast<size_t>(cacheSize_.load()));
	flushDeallocations();

	if (executeOnce(query))
	{
		return;
	}
	// Outside a transaction of the script's own, nothing has been applied: try once more.
	if (retryable(query.sqlState) && PQstatus(conn_) == CONNECTION_OK && PQtransactionStatus(conn_) == PQTRANS_IDLE)
	{
		query.error.clear();
		query.sqlState.clear();
		query.failedSql.clear();
		executeOnce(query);
	}
}

bool Connection::executeOnce(Query& query)
{
	query.clearResults();

	if (query.transaction)
	{
		PGresult* res = simpleExec("BEGIN");
		if (!succeeded(res))
		{
			fail(query, res, "BEGIN");
			return false;
		}
		PQclear(res);
	}

	for (const Statement& statement : query.statements)
	{
		PGresult* res = exec(statement);
		if (!succeeded(res))
		{
			fail(query, res, statement.sql);
			if (statement.prepare && retryable(query.sqlState))
			{
				// 0A000 leaves the stale statement on the server; 26000 means it is already gone.
				forgetStatement(statement.sql, query.sqlState == "0A000");
			}
			if (query.transaction)
			{
				rollbackIfNeeded();
			}
			return false;
		}
		query.results.push_back(res);
	}

	if (query.transaction)
	{
		PGresult* res = simpleExec("COMMIT");
		// COMMIT of an already aborted transaction "succeeds" with the tag ROLLBACK.
		bool committed = succeeded(res) && std::string(PQcmdStatus(res)) == "COMMIT";
		if (!committed)
		{
			bool lost = PQstatus(conn_) != CONNECTION_OK;
			fail(query, res, "COMMIT");
			if (lost)
			{
				// The server may or may not have committed before the connection dropped.
				query.sqlState = "08007"; // transaction_resolution_unknown
			}
			rollbackIfNeeded();
			return false;
		}
		PQclear(res);
	}
	return true;
}

PGresult* Connection::simpleExec(const char* sql)
{
	return PQexecParams(conn_, sql, 0, nullptr, nullptr, nullptr, nullptr, 0);
}

PGresult* Connection::exec(const Statement& statement)
{
	std::vector<const char*> values;
	values.reserve(statement.params.size());
	for (const QueryParam& p : statement.params)
	{
		values.push_back(p.isNull ? nullptr : p.value.c_str());
	}
	int count = static_cast<int>(values.size());

	size_t limit = static_cast<size_t>(cacheSize_.load());
	if (!statement.prepare || limit == 0)
	{
		// Text-format parameters; Postgres infers the types from the statement.
		return PQexecParams(conn_, statement.sql.c_str(), count, nullptr, values.data(), nullptr, nullptr, 0);
	}

	auto it = statements_.find(statement.sql);
	if (it != statements_.end())
	{
		lru_.splice(lru_.begin(), lru_, it->second.lru);
	}
	else
	{
		trimStatementCache(limit - 1);
		std::string name = "omp_" + std::to_string(nextStatement_++);
		PGresult* prepared = PQprepare(conn_, name.c_str(), statement.sql.c_str(), count, nullptr);
		if (!succeeded(prepared))
		{
			// Same error the statement itself would give (syntax, unknown table, ...).
			return prepared;
		}
		PQclear(prepared);
		lru_.push_front(statement.sql);
		it = statements_.emplace(statement.sql, CachedStatement { std::move(name), lru_.begin() }).first;
	}
	return PQexecPrepared(conn_, it->second.name.c_str(), count, values.data(), nullptr, nullptr, 0);
}

void Connection::fail(Query& query, PGresult* res, const std::string& sql)
{
	// Results of earlier statements in a failed transaction were rolled back; don't hand them out.
	query.clearResults();
	const char* state = res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : nullptr;
	query.sqlState = state ? state : "";
	// The primary message is one line; the full message adds "LINE 1: ..." context.
	const char* primary = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY) : nullptr;
	query.error = primary ? primary : PQerrorMessage(conn_);
	if (query.error.empty())
	{
		query.error = "unknown error";
	}
	query.failedSql = sql;
	if (res)
	{
		PQclear(res);
	}
	if (PQstatus(conn_) != CONNECTION_OK)
	{
		connected_ = false;
		if (query.sqlState.empty())
		{
			query.sqlState = "08006";
		}
	}
}

void Connection::rollbackIfNeeded()
{
	if (PQstatus(conn_) == CONNECTION_OK && PQtransactionStatus(conn_) != PQTRANS_IDLE)
	{
		PQclear(simpleExec("ROLLBACK"));
	}
}

void Connection::forgetStatement(const std::string& sql, bool deallocate)
{
	auto it = statements_.find(sql);
	if (it == statements_.end())
	{
		return;
	}
	if (deallocate)
	{
		toDeallocate_.push_back(std::move(it->second.name));
	}
	lru_.erase(it->second.lru);
	statements_.erase(it);
}

void Connection::trimStatementCache(size_t limit)
{
	while (lru_.size() > limit)
	{
		forgetStatement(lru_.back(), true);
	}
}

void Connection::flushDeallocations()
{
	// DEALLOCATE fails inside an aborted transaction, so only run it between transactions.
	if (toDeallocate_.empty() || PQtransactionStatus(conn_) != PQTRANS_IDLE)
	{
		return;
	}
	std::string sql;
	for (const std::string& name : toDeallocate_)
	{
		sql += "DEALLOCATE " + name + ";";
	}
	toDeallocate_.clear();
	// Several commands need the simple protocol; the names are our own, never script input.
	PQclear(PQexec(conn_, sql.c_str()));
}

void Connection::clearStatementCache()
{
	statements_.clear();
	lru_.clear();
	toDeallocate_.clear();
}
