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

// Errors from a cached prepared statement that mean it went stale, raised
// before it ran: 26000 = the statement is gone (DISCARD ALL, pooler),
// 0A000 = "cached plan must not change result type" after a schema change.
bool staleStatement(const std::string& sqlState)
{
	return sqlState == "26000" || sqlState == "0A000";
}

// PQcancel opens a new connection to the server, which takes as long as the
// network allows, so it runs on its own thread instead of holding up close().
void cancelInBackground(PGcancel* cancel)
{
	if (cancel == nullptr)
	{
		return;
	}
	std::thread([cancel] {
		char error[256];
		PQcancel(cancel, error, sizeof error);
		PQfreeCancel(cancel);
	}).detach();
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
	std::vector<const char*> keys;
	std::vector<const char*> values;
	auto add = [&](const char* key, const char* value) {
		keys.push_back(key);
		values.push_back(value);
	};
	// SA-MP clients send Windows-1252 bytes, not UTF-8, so let the server convert
	// unless the conninfo or PGCLIENTENCODING already chose an encoding.
	if (std::getenv("PGCLIENTENCODING") == nullptr)
	{
		add("client_encoding", "WIN1252");
	}
	// libpq waits forever by default, which would hang pg_connect and every reconnect.
	if (std::getenv("PGCONNECT_TIMEOUT") == nullptr)
	{
		add("connect_timeout", "10");
	}
	add("fallback_application_name", "omp-postgres");
	// Notice a vanished server or network within about a minute; with the OS
	// defaults a query can hang for 15 minutes or more.
	add("keepalives_idle", "30");
	add("keepalives_interval", "10");
	add("keepalives_count", "3");
	if (PQlibVersion() >= 120000)
	{
		add("tcp_user_timeout", "30000");
	}
	add("dbname", conninfo.c_str());
	add(nullptr, nullptr);

	PGconn* conn = PQconnectdbParams(keys.data(), values.data(), 1);
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
	, cancel_(PQgetCancel(conn))
{
	worker_ = std::thread(&Connection::run, this);
}

Connection::~Connection()
{
	close();
	if (cancel_)
	{
		PQfreeCancel(cancel_);
	}
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

void Connection::close(std::chrono::milliseconds timeout)
{
	if (!worker_.joinable())
	{
		return;
	}
	{
		std::unique_lock<std::mutex> lock(mutex_);
		stopping_ = true;
		wake_.notify_one();
		while (!finished_)
		{
			if (busy_ && !abandon_ && std::chrono::steady_clock::now() - busySince_ >= timeout)
			{
				abandon_ = true;
				cancelInBackground(cancel_);
				cancel_ = nullptr;
			}
			idle_.wait_for(lock, std::chrono::milliseconds(50));
		}
	}
	worker_.join();
}

size_t Connection::pending() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return queue_.size() + (busy_ ? 1 : 0);
}

void Connection::run()
{
	for (;;)
	{
		QueryPtr query;
		bool abandoned;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
			// Drain the queue before stopping, so saves sent during shutdown still land.
			if (queue_.empty())
			{
				finished_ = true;
				idle_.notify_all();
				return;
			}
			query = std::move(queue_.front());
			queue_.pop_front();
			busy_ = true;
			busySince_ = std::chrono::steady_clock::now();
			abandoned = abandon_;
		}
		if (abandoned)
		{
			query->error = "connection closed before the query ran";
			query->sqlState = "08003"; // connection_does_not_exist
		}
		else
		{
			execute(*query);
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			busy_ = false;
		}
		completions_.push(std::move(query));
	}
}

bool Connection::ensureConnected(std::string& error)
{
	if (PQstatus(conn_) == CONNECTION_OK)
	{
		// Notice a connection the server closed while idle (restart, pg_terminate_backend,
		// idle_session_timeout) before sending anything on it. libpq's socket is
		// non-blocking, so this never waits: the first call reads what the server
		// sent, the second sees the end of the stream.
		PQconsumeInput(conn_);
		PQconsumeInput(conn_);
		discardNotifications();
		if (PQstatus(conn_) == CONNECTION_OK)
		{
			return true;
		}
		connected_ = false;
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
		refreshCancel();
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

	if (executeOnce(query) || !retrySafe_)
	{
		return;
	}
	// Nothing was applied, so run it once more (on a new connection if it dropped).
	std::string error;
	if (!ensureConnected(error))
	{
		return;
	}
	// A transaction of the script's own (BEGIN sent with pg_query) is still open; don't run outside it.
	if (PQtransactionStatus(conn_) != PQTRANS_IDLE)
	{
		return;
	}
	query.error.clear();
	query.sqlState.clear();
	query.failedSql.clear();
	executeOnce(query);
}

bool Connection::executeOnce(Query& query)
{
	query.clearResults();
	retrySafe_ = false;

	if (query.transaction)
	{
		PGresult* res = simpleExec("BEGIN");
		if (!succeeded(res))
		{
			fail(query, res, "BEGIN");
			retrySafe_ = PQstatus(conn_) != CONNECTION_OK;
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
			if (lastExecCached_ && staleStatement(query.sqlState))
			{
				// 0A000 leaves the stale statement on the server; 26000 means it is already gone.
				forgetStatement(statement.sql, query.sqlState == "0A000");
				retrySafe_ = true;
			}
			if (query.transaction)
			{
				// The server rolls back a transaction whose connection drops before COMMIT.
				if (PQstatus(conn_) != CONNECTION_OK)
				{
					retrySafe_ = true;
				}
				rollbackIfNeeded();
			}
			else if (PQstatus(conn_) != CONNECTION_OK && query.sqlState == "08006")
			{
				// The statement was sent, so it may have run and committed before the drop.
				query.sqlState = "08007"; // transaction_resolution_unknown
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
	lastExecCached_ = false;

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
		lastExecCached_ = true;
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
	// One command each: in a single multi-command string, one failure would skip the rest.
	// The names are our own, never script input.
	for (const std::string& name : toDeallocate_)
	{
		PQclear(simpleExec(("DEALLOCATE " + name).c_str()));
	}
	toDeallocate_.clear();
}

void Connection::refreshCancel()
{
	// The cancel key changes with every new server session.
	PGcancel* fresh = PQgetCancel(conn_);
	std::lock_guard<std::mutex> lock(mutex_);
	if (cancel_)
	{
		PQfreeCancel(cancel_);
	}
	cancel_ = fresh;
}

void Connection::discardNotifications()
{
	// Only arrive after a LISTEN sent with pg_query; nothing reads them, so don't let them pile up.
	while (PGnotify* notify = PQnotifies(conn_))
	{
		PQfreemem(notify);
	}
}

void Connection::clearStatementCache()
{
	statements_.clear();
	lru_.clear();
	toDeallocate_.clear();
}
